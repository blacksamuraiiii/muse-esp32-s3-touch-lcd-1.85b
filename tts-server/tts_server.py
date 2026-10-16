#!/usr/bin/env python3
"""
Edge TTS bridge for muse-gadget-sdk firmware (MUSE_TTS_URL).

The firmware does:  GET /tts?voice=<voice>&text=<url-encoded text>
and expects an MP3 back. Long replies are split into ~180-byte chunks
by the firmware, so each request is short.

Borrowed from timmy2048/muse-gadget-185b's tools/tts_wg_proxy.py (Azure Speech):
  * rate / pitch prosody knobs (+8% / +4% there) - edge-tts takes them directly,
    so no SSML; Azure's express-as *style* has no edge-tts equivalent.
  * answer 502 unless the payload really is MP3 (0xFF sync or ID3 header), so an
    upstream error body never reaches the board's decoder.
  * MAX_TEXT_LEN aligned with the firmware's 180-byte chunking.
  * /health answers with the effective settings instead of a bare "ok".
  * accept both `q` (his parameter name) and `text` (ours).
Added here: an LRU cache, so a repeated reply is served without re-synthesising.

Install:  pip install flask edge-tts
Run:      python tts_server.py            # listens on 0.0.0.0:5050
Test:     curl "http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD" -o t.mp3
          curl "http://127.0.0.1:5050/health"

Firmware side (in the board's build sdkconfig, then rebuild + flash):
    CONFIG_MUSE_TTS_URL="http://<this-machine-lan-ip>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
"""

import asyncio
import json
import logging
import os
import queue
import re
import threading
import time
from collections import OrderedDict

import edge_tts
from flask import Flask, Response, request

app = Flask(__name__)
log = logging.getLogger("tts")

DEFAULT_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
# timmy2048's proxy reads at +8% / +4% for a livelier voice; override with env
# or per request via ?rate=/&pitch=. NOTE: edge-tts wants pitch in Hz, not the
# Azure-style percent, so a value must match [+-]<digits>Hz (rate/volume are %).
DEFAULT_RATE = os.environ.get("TTS_RATE", "+8%")
DEFAULT_PITCH = os.environ.get("TTS_PITCH", "+0Hz")

_RATE_RE = re.compile(r"^[+-]\d+%$")
_PITCH_RE = re.compile(r"^[+-]\d+Hz$")
# Keep in sync with the firmware's per-request chunking; longer single
# requests are slower to start playing.
MAX_TEXT_LEN = int(os.environ.get("TTS_MAX_CHARS", "180"))
CACHE_MAX_ITEMS = int(os.environ.get("TTS_CACHE_ITEMS", "64"))
CACHE_MAX_BYTES = int(os.environ.get("TTS_CACHE_BYTES", str(8 * 1024 * 1024)))
# Streaming sends audio as it is synthesised, so playback starts ~1 s in instead
# of after the whole reply. stream=0 asks for the old buffered behaviour.
STREAM_DEFAULT = os.environ.get("TTS_STREAM", "1") != "0"
# Bytes to hold before answering 200: enough to prove it really is MP3.
STREAM_HEAD_BYTES = 1024
STREAM_FIRST_TIMEOUT = 20.0

_cache = OrderedDict()   # (voice, rate, pitch, text) -> mp3 bytes
_cache_bytes = 0


def clean(text: str) -> str:
    """Drops what shouldn't be read aloud: code, URLs and Markdown marks."""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links/images: keep text
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[*_`#>|~]+", "", text)
    text = re.sub(r"^\s*[-+]\s+", "", text, flags=re.M)  # list bullets
    return re.sub(r"\s+", " ", text).strip()[:MAX_TEXT_LEN]


def is_mp3(data: bytes) -> bool:
    """0xFF frame sync or an ID3 tag; anything else is an upstream error body."""
    return len(data) > 64 and (data[0] == 0xFF or data[:3] == b"ID3")


async def _synth(text: str, voice: str, rate: str, pitch: str) -> bytes:
    audio = bytearray()
    async for chunk in edge_tts.Communicate(
        text, voice, rate=rate, pitch=pitch
    ).stream():
        if chunk["type"] == "audio":
            audio += chunk["data"]
    return bytes(audio)


def _start_synth_thread(text, voice, rate, pitch, out: "queue.Queue"):
    """Feed edge-tts audio into `out` from a thread; None ends it, an Exception reports it."""

    async def run():
        try:
            async for chunk in edge_tts.Communicate(
                text, voice, rate=rate, pitch=pitch
            ).stream():
                if chunk["type"] == "audio":
                    out.put(chunk["data"])
        except Exception as e:  # noqa: BLE001 - handed to the view, reported as 502
            out.put(e)
        finally:
            out.put(None)

    threading.Thread(target=lambda: asyncio.run(run()), daemon=True).start()


def _stream_reply(key, text, voice, rate, pitch, started):
    """Generator: yield audio as it arrives, cache the whole thing at the end."""
    out: queue.Queue = queue.Queue()
    _start_synth_thread(text, voice, rate, pitch, out)

    head = bytearray()
    deadline = time.monotonic() + STREAM_FIRST_TIMEOUT
    while len(head) < STREAM_HEAD_BYTES:
        try:
            item = out.get(timeout=max(0.1, deadline - time.monotonic()))
        except queue.Empty:
            raise RuntimeError("upstream timed out before the first audio")
        if item is None:
            break
        if isinstance(item, Exception):
            raise item
        head += item
    if not is_mp3(bytes(head)):
        raise RuntimeError(
            "upstream did not return MP3: %d bytes head=%r" % (len(head), bytes(head[:16]))
        )
    log.info("first bytes after %.2fs (%d bytes)", time.monotonic() - started, len(head))

    def gen():
        buf = bytearray(head)
        yield bytes(head)
        while True:
            item = out.get()
            if item is None:
                break
            if isinstance(item, Exception):
                log.warning("stream ended with %r", item)
                break
            buf += item
            yield item
        if is_mp3(bytes(buf)):
            cache_put(key, bytes(buf))
        log.info("streamed %d bytes in %.2fs (%s rate=%s pitch=%s)",
                 len(buf), time.monotonic() - started, voice, rate, pitch)

    return gen()


def cache_get(key):
    hit = _cache.get(key)
    if hit is not None:
        _cache.move_to_end(key)
    return hit


def cache_put(key, data: bytes):
    global _cache_bytes
    _cache[key] = data
    _cache_bytes += len(data)
    while _cache and (len(_cache) > CACHE_MAX_ITEMS or _cache_bytes > CACHE_MAX_BYTES):
        _, old = _cache.popitem(last=False)
        _cache_bytes -= len(old)


@app.route("/tts")
def tts():
    text = request.args.get("text") or request.args.get("q") or ""
    voice = request.args.get("voice") or DEFAULT_VOICE
    rate = request.args.get("rate") or DEFAULT_RATE
    pitch = request.args.get("pitch") or DEFAULT_PITCH
    # A malformed knob must not 502 the reply: fall back to the default.
    if not _RATE_RE.match(rate):
        log.warning("ignoring bad rate %r", rate)
        rate = DEFAULT_RATE
    if not _PITCH_RE.match(pitch):
        log.warning("ignoring bad pitch %r", pitch)
        pitch = DEFAULT_PITCH
    if not text:
        return "missing text", 400
    text = clean(text)
    if not text:
        return "empty after cleaning", 400

    key = (voice, rate, pitch, text)
    hit = cache_get(key)
    if hit is not None:
        log.info("cache hit %d chars -> %d bytes (%s)", len(text), len(hit), voice)
        resp = Response(hit, mimetype="audio/mpeg")
        resp.headers["X-TTS-Cache"] = "hit"
        return resp

    started = time.monotonic()
    if (request.args.get("stream") or ("1" if STREAM_DEFAULT else "0")) != "0":
        try:
            gen = _stream_reply(key, text, voice, rate, pitch, started)
        except Exception as e:  # noqa: BLE001
            log.warning("stream failed: %s", e)
            return f"tts failed: {e}", 502
        resp = Response(gen, mimetype="audio/mpeg")
        resp.headers["X-TTS-Cache"] = "miss"
        resp.headers["X-TTS-Stream"] = "1"
        return resp

    try:
        audio = asyncio.run(_synth(text, voice, rate, pitch))
    except Exception as e:  # noqa: BLE001 - report it, don't crash the server
        log.exception("synthesis failed")
        return f"tts failed: {e}", 502
    if not is_mp3(audio):
        log.warning("upstream did not return MP3: %d bytes head=%r",
                    len(audio), audio[:16])
        return "upstream did not return MP3", 502
    cache_put(key, audio)
    log.info("served %d chars -> %d bytes (%s rate=%s pitch=%s) in %.2fs",
             len(text), len(audio), voice, rate, pitch, time.monotonic() - started)
    resp = Response(audio, mimetype="audio/mpeg")
    resp.headers["X-TTS-Cache"] = "miss"
    return resp


@app.route("/health")
def health():
    return Response(
        json.dumps(
            {
                "service": "muse-tts",
                "voice": DEFAULT_VOICE,
                "rate": DEFAULT_RATE,
                "pitch": DEFAULT_PITCH,
                "max_chars": MAX_TEXT_LEN,
                "cache_items": len(_cache),
                "cache_bytes": _cache_bytes,
            },
            ensure_ascii=False,
        ),
        mimetype="application/json",
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    # 0.0.0.0 so the ESP32 on the LAN can reach it.
    app.run(host="0.0.0.0", port=5050, threaded=True)

#!/usr/bin/env python3
"""
Azure Speech TTS bridge for muse-gadget-sdk firmware (MUSE_TTS_URL), with the
edge-tts bridge kept as a fallback.

The firmware does:  GET /tts?voice=<voice>&text=<url-encoded text>
and expects an MP3 back. Long replies are split into ~180-byte chunks
by the firmware, so each request is short.

Backends (TTS_BACKEND, default azure):
  azure  POST SSML to https://<region>.tts.speech.microsoft.com/cognitiveservices/v1
         with the Ocp-Apim-Subscription-Key header. Shape, prosody and the
         punctuation breaks are ported from timmy2048/muse-gadget-185b's
         tools/tts_wg_proxy.py; style=express-as (mstts) is Azure-only.
         Output format audio-24khz-48kbitrate-mono-mp3 is what the board
         already plays (its log line reads back "24000 Hz, 1 ch, 48 kbps").
  edge   the previous edge-tts path, still here for when the Azure key is out of
         quota or the service is down. NOTE edge-tts wants pitch in Hz while
         Azure wants percent, so pitch is validated per backend.

Falling back: when the chosen backend fails before any audio is delivered and
TTS_FALLBACK names the other one, the request is retried on it.

Install:  pip install flask waitress edge-tts
Run:      python tts_server.py            # listens on 0.0.0.0:5050, served by waitress
Test:     curl "http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD" -o t.mp3
          curl "http://127.0.0.1:5050/health"

Server: waitress by default. The Flask/werkzeug dev server (TTS_SERVER=werkzeug)
truncated the body for the board: it read ~1440 bytes of a 51264-byte reply and
then saw EOF, on both the chunked and the Content-Length path, while a raw-socket
server on the same port, same container image and same payload delivered all of
it. Serve with waitress; keep the dev server only for quick local pokes.

Latency: the upstream connection is kept alive per worker thread. Measured from
the dev machine, a cold TLS connection to the Speech endpoint takes 1.20-1.39 s
to the first audio byte and a warm one 0.45-0.53 s, which is most of the delay
the board sees. Two smaller knobs (TTS_STREAM_HEAD, TTS_SEND_BYTES) hold back
less audio before the response starts. `style` costs ~0.14 s; set
AZURE_SPEECH_STYLE= to drop it.

Regression hooks (TTS_TEST_HOOKS=1, off by default): ?text=BADPACKET answers 200
with an HTML body and ?text=BADSTATUS answers 500, the two shapes a broken
upstream has, for the firmware's non-MP3 gate. TTS_FORCE_BAD=1 makes every reply
that 200-with-HTML one, so the gate can be exercised from the device itself.

Secrets come from the environment, or from a .env beside this file
(AZURE_SPEECH_KEY / AZURE_SPEECH_REGION). .env is not in the repo.

Firmware side (in the board's build sdkconfig, then rebuild + flash):
    CONFIG_MUSE_TTS_URL="http://<this-machine-lan-ip>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
"""

import asyncio
import http.client
import json
import logging
import os
import queue
import re
import ssl
import threading
import time
from collections import OrderedDict
from xml.sax.saxutils import escape

from flask import Flask, Response, request

app = Flask(__name__)
log = logging.getLogger("tts")


def _load_env(path):
    """Minimal .env reader (no python-dotenv dependency); real env wins."""
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())
    except FileNotFoundError:
        pass


_load_env(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))

BACKEND = os.environ.get("TTS_BACKEND", "azure").strip().lower()
# The other backend to retry on when the chosen one delivers nothing. Off by
# default: edge-tts currently answers 403 (its Sec-MS-GEC token handshake broke),
# and a fallback that always fails just adds its timeout to the 502.
FALLBACK = os.environ.get("TTS_FALLBACK", "").strip().lower()
if FALLBACK == BACKEND:
    FALLBACK = ""

AZURE_KEY = os.environ.get("AZURE_SPEECH_KEY", "").strip()
AZURE_REGION = os.environ.get("AZURE_SPEECH_REGION", "eastasia").strip()
AZURE_STYLE = os.environ.get("AZURE_SPEECH_STYLE", "friendly").strip()
AZURE_STYLE_DEGREE = os.environ.get("AZURE_SPEECH_STYLE_DEGREE", "1.2").strip()
AZURE_FORMAT = os.environ.get(
    "AZURE_SPEECH_FORMAT", "audio-24khz-48kbitrate-mono-mp3"
).strip()

DEFAULT_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
# The board's CONFIG_MUSE_TTS_URL carries its own voice=... query param, and that
# would win over TTS_VOICE. Set TTS_FORCE_VOICE to try another timbre without
# rebuilding/reflashing the firmware (e.g. zh-CN-XiaoyiNeural).
FORCE_VOICE = os.environ.get("TTS_FORCE_VOICE", "").strip()
# timmy2048's proxy reads at +8% / +4% for a livelier voice; override with env
# or per request via ?rate=/&pitch=. Units differ per backend, see file header.
DEFAULT_RATE = os.environ.get("TTS_RATE", "+8%")
DEFAULT_PITCH = os.environ.get("TTS_PITCH", "+4%")

_RATE_RE = re.compile(r"^[+-]\d+%$")
_PITCH_RE = {"azure": re.compile(r"^[+-]\d+%$"), "edge": re.compile(r"^[+-]\d+Hz$")}
# Keep in sync with the firmware's per-request chunking; longer single
# requests are slower to start playing.
MAX_TEXT_LEN = int(os.environ.get("TTS_MAX_CHARS", "180"))
# Port to listen on; the firmware's CONFIG_MUSE_TTS_URL points at 5050.
PORT = int(os.environ.get("TTS_PORT", "5050"))
# "waitress" (default, HTTP/1.1 server) or "werkzeug" (Flask dev server, local
# poking only). See the file header for why the dev server is not the default.
SERVER = os.environ.get("TTS_SERVER", "waitress").strip().lower()
CACHE_MAX_ITEMS = int(os.environ.get("TTS_CACHE_ITEMS", "64"))
CACHE_MAX_BYTES = int(os.environ.get("TTS_CACHE_BYTES", str(8 * 1024 * 1024)))
# Streaming sends audio as it is synthesised, so playback starts ~1 s in instead
# of after the whole reply. stream=0 asks for the old buffered behaviour.
STREAM_DEFAULT = os.environ.get("TTS_STREAM", "1") != "0"
# Bytes to hold before answering 200: enough to prove it really is MP3. Keep it
# small: the board only needs the first frame, and every byte held here is
# playback delay.
STREAM_HEAD_BYTES = int(os.environ.get("TTS_STREAM_HEAD", "512"))
STREAM_FIRST_TIMEOUT = 20.0
AZURE_TIMEOUT = 20.0
EDGE_FAIL_HINT = "the Azure key may be out of quota or the region wrong"
# The board's own gate needs 8 bytes; waitress's output buffer is the other
# half of the delay, so both thresholds are lowered together (see __main__).
SEND_BYTES = int(os.environ.get("TTS_SEND_BYTES", "512"))
# Regression hook for the firmware's bad-payload gate: ?text=BADPACKET answers
# 500 with an HTML body, exactly what a failing upstream looks like. Off unless
# asked for, so a production deployment can never trip it by accident.
TEST_HOOKS = os.environ.get("TTS_TEST_HOOKS", "") == "1"
# With the hooks on, this makes every reply the bad payload, so the board's
# gate can be exercised without controlling what the device says.
FORCE_BAD = os.environ.get("TTS_FORCE_BAD", "") == "1"


_cache = OrderedDict()   # (backend, voice, rate, pitch, text) -> mp3 bytes
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


# ---------------------------------------------------------------- Azure Speech

def _ssml(text: str, voice: str, rate: str, pitch: str) -> str:
    """SSML with short breaks at sentence punctuation (from tts_wg_proxy.py)."""
    parts = []
    buf = []
    for ch in text:
        if ch in "。！？!?；;":
            buf.append(ch)
            parts.append(escape("".join(buf)))
            parts.append("<break time='280ms'/>")
            buf = []
        elif ch in "，、,":
            buf.append(ch)
            parts.append(escape("".join(buf)))
            parts.append("<break time='120ms'/>")
            buf = []
        else:
            buf.append(ch)
    if buf:
        parts.append(escape("".join(buf)))
    inner = "<prosody rate='%s' pitch='%s'>%s</prosody>" % (
        escape(rate, {"'": "&apos;"}),
        escape(pitch, {"'": "&apos;"}),
        "".join(parts),
    )
    if AZURE_STYLE:
        inner = (
            "<mstts:express-as style='%s' styledegree='%s'>%s</mstts:express-as>"
            % (escape(AZURE_STYLE, {"'": "&apos;"}),
               escape(AZURE_STYLE_DEGREE, {"'": "&apos;"}), inner)
        )
    return (
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
        "xmlns:mstts='https://www.w3.org/2001/mstts' xml:lang='zh-CN'>"
        "<voice xml:lang='zh-CN' name='%s'>%s</voice></speak>"
        % (escape(voice, {"'": "&apos;"}), inner)
    )


_SSL_CTX = ssl.create_default_context()
# Kept-alive upstream connections. A pool, not a thread-local: the streaming
# worker is a fresh thread per request, so per-thread state would never be
# reused (that mistake measured 1.3 s per request instead of 0.5 s).
_pool: "queue.LifoQueue" = queue.LifoQueue()
_pool_open = 0                      # sockets created and not yet closed
_pool_lock = threading.Lock()
AZURE_POOL_MAX = int(os.environ.get("TTS_AZURE_POOL", "6"))


def _azure_acquire():
    """(connection, reused): a warm one from the pool, or a new one."""
    global _pool_open
    try:
        return _pool.get_nowait(), True
    except queue.Empty:
        pass
    with _pool_lock:
        _pool_open += 1
    host = "%s.tts.speech.microsoft.com" % AZURE_REGION
    return http.client.HTTPSConnection(host, timeout=AZURE_TIMEOUT, context=_SSL_CTX), False


def _azure_release(conn, reusable):
    """Back to the pool when its body was read to the end, else closed."""
    global _pool_open
    if reusable:
        if _pool.qsize() < AZURE_POOL_MAX:
            _pool.put(conn)
            return
    try:
        conn.close()
    except Exception:  # noqa: BLE001 - closing a broken socket
        pass
    with _pool_lock:
        _pool_open -= 1


class _AzureResponse:
    """The upstream response and the connection it came from.

    Reading it to the end hands the connection back to the pool; leaving it
    half-read drops the socket instead of poisoning the next request."""

    def __init__(self, conn, resp):
        self._conn = conn
        self._resp = resp
        self._done = False

    def read(self, n=-1):
        data = self._resp.read(n)
        if not data or n < 0:
            self._done = True
        return data

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        conn, self._conn = self._conn, None
        try:
            self._resp.close()
        except Exception:  # noqa: BLE001 - already closed
            pass
        if conn is not None:
            _azure_release(conn, self._done)


def _azure_request(text: str, voice: str, rate: str, pitch: str):
    """The open response object; raises on a non-200 so callers can fall back."""
    if not AZURE_KEY:
        raise RuntimeError("AZURE_SPEECH_KEY is not set")
    body = _ssml(text, voice, rate, pitch).encode("utf-8")
    headers = {
        "Ocp-Apim-Subscription-Key": AZURE_KEY,
        "Content-Type": "application/ssml+xml",
        "X-Microsoft-OutputFormat": AZURE_FORMAT,
        "User-Agent": "muse-tts-proxy",
    }
    last = None
    for attempt in (0, 1):
        conn, reused = _azure_acquire()
        try:
            conn.request("POST", "/cognitiveservices/v1", body, headers)
            resp = conn.getresponse()
        except (http.client.HTTPException, OSError) as e:
            # A pooled socket the service closed while it was idle looks
            # exactly like this. Retry once on a new one; a fresh connection
            # that fails is a real failure.
            _azure_release(conn, False)
            last = e
            if not reused or attempt:
                break
            continue
        if resp.status != 200:
            detail = resp.read()[:200].decode("utf-8", "replace")
            _azure_release(conn, False)   # the body is partial: never reuse
            raise RuntimeError("azure http %d: %s" % (resp.status, detail))
        return _AzureResponse(conn, resp)
    raise RuntimeError("azure connection failed: %s" % last)


def _synth_azure(text, voice, rate, pitch) -> bytes:
    with _azure_request(text, voice, rate, pitch) as r:
        return r.read()


def _stream_azure(text, voice, rate, pitch, out: "queue.Queue"):
    try:
        with _azure_request(text, voice, rate, pitch) as r:
            while True:
                chunk = r.read(4096)
                if not chunk:
                    break
                out.put(chunk)
    except Exception as e:  # noqa: BLE001 - handed to the view, reported as 502
        out.put(e)
    finally:
        out.put(None)


# ------------------------------------------------------------------- edge-tts

async def _edge_chunks(text, voice, rate, pitch):
    import edge_tts

    async for chunk in edge_tts.Communicate(text, voice, rate=rate, pitch=pitch).stream():
        if chunk["type"] == "audio":
            yield chunk["data"]


def _synth_edge(text, voice, rate, pitch) -> bytes:
    async def run():
        audio = bytearray()
        async for data in _edge_chunks(text, voice, rate, pitch):
            audio += data
        return bytes(audio)

    return asyncio.run(run())


def _stream_edge(text, voice, rate, pitch, out: "queue.Queue"):
    async def run():
        try:
            async for data in _edge_chunks(text, voice, rate, pitch):
                out.put(data)
        except Exception as e:  # noqa: BLE001 - handed to the view, reported as 502
            out.put(e)
        finally:
            out.put(None)

    threading.Thread(target=lambda: asyncio.run(run()), daemon=True).start()


# ------------------------------------------------------------------- plumbing

SYNTH = {"azure": _synth_azure, "edge": _synth_edge}
STREAM = {"azure": _stream_azure, "edge": _stream_edge}


def _start_stream_thread(backend, text, voice, rate, pitch, out: "queue.Queue"):
    if backend == "edge":
        _stream_edge(text, voice, rate, pitch, out)   # starts its own thread
    else:
        threading.Thread(
            target=STREAM[backend], args=(text, voice, rate, pitch, out), daemon=True
        ).start()


def _stream_reply(key, text, voice, rate, pitch, started, backend):
    """Generator: yield audio as it arrives, cache the whole thing at the end.

    Raises before the first yield when the backend delivers nothing usable, so
    the caller can still answer 502 (or retry on the fallback backend)."""
    out: queue.Queue = queue.Queue()
    _start_stream_thread(backend, text, voice, rate, pitch, out)

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
        log.info("streamed %d bytes in %.2fs (%s %s rate=%s pitch=%s)",
                 len(buf), time.monotonic() - started, backend, voice, rate, pitch)

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


def _synth(backend, text, voice, rate, pitch) -> bytes:
    return SYNTH[backend](text, voice, rate, pitch)


@app.route("/tts")
def tts():
    text = request.args.get("text") or request.args.get("q") or ""
    voice = FORCE_VOICE or request.args.get("voice") or DEFAULT_VOICE
    rate = request.args.get("rate") or DEFAULT_RATE
    pitch = request.args.get("pitch") or DEFAULT_PITCH
    # A malformed knob must not 502 the reply: fall back to the default.
    if not _RATE_RE.match(rate):
        log.warning("ignoring bad rate %r", rate)
        rate = DEFAULT_RATE
    if not _PITCH_RE[BACKEND].match(pitch):
        log.warning("ignoring bad pitch %r for %s", pitch, BACKEND)
        pitch = DEFAULT_PITCH
    if not text:
        return "missing text", 400
    text = clean(text)
    if not text:
        return "empty after cleaning", 400
    if TEST_HOOKS and (text.upper() in ("BADPACKET", "BADSTATUS") or FORCE_BAD):
        # 200 with an HTML body is what a misbehaving proxy in front of the
        # service looks like, and the only way to exercise the firmware's
        # non-MP3 gate; 500 is what a failing upstream looks like.
        status = 500 if text.upper() == "BADSTATUS" else 200
        log.info("test hook: answering %d with HTML", status)
        return Response("<html><body>500 fake error</body></html>", status=status,
                        mimetype="text/html")

    key = (BACKEND, voice, rate, pitch, text)
    hit = cache_get(key)
    if hit is not None:
        log.info("cache hit %d chars -> %d bytes (%s)", len(text), len(hit), voice)
        resp = Response(hit, mimetype="audio/mpeg")
        resp.headers["X-TTS-Cache"] = "hit"
        resp.headers["X-TTS-Backend"] = BACKEND
        return resp

    started = time.monotonic()
    streaming = (request.args.get("stream") or ("1" if STREAM_DEFAULT else "0")) != "0"
    try:
        if streaming:
            gen = _stream_reply(key, text, voice, rate, pitch, started, BACKEND)
        else:
            audio = _synth(BACKEND, text, voice, rate, pitch)
            if not is_mp3(audio):
                raise RuntimeError(
                    "upstream did not return MP3: %d bytes head=%r" % (len(audio), audio[:16])
                )
    except Exception as e:  # noqa: BLE001 - report it, don't crash the server
        log.warning("%s failed: %s (%s)", BACKEND, e, EDGE_FAIL_HINT if BACKEND == "azure" else "")
        if not FALLBACK:
            return "tts failed: %s" % e, 502
        # Nothing reached the board yet, so the other backend can still serve it.
        fb_key = (FALLBACK, voice, rate, pitch, text)
        fb_rate, fb_pitch = _fallback_prosody(rate, pitch)
        log.warning("retrying on %s (rate=%s pitch=%s)", FALLBACK, fb_rate, fb_pitch)
        try:
            if streaming:
                gen = _stream_reply(fb_key, text, voice, fb_rate, fb_pitch, started, FALLBACK)
            else:
                audio = _synth(FALLBACK, text, voice, fb_rate, fb_pitch)
                if not is_mp3(audio):
                    raise RuntimeError("fallback payload is not MP3")
                cache_put(fb_key, audio)
                gen = None
        except Exception as e2:  # noqa: BLE001
            log.exception("fallback %s failed too", FALLBACK)
            return "tts failed: %s / fallback %s: %s" % (e, FALLBACK, e2), 502
        if gen is None:
            resp = Response(audio, mimetype="audio/mpeg")
            resp.headers["X-TTS-Backend"] = FALLBACK
            resp.headers["X-TTS-Cache"] = "miss"
            return resp
        resp = Response(gen, mimetype="audio/mpeg")
        resp.headers["X-TTS-Cache"] = "miss"
        resp.headers["X-TTS-Stream"] = "1"
        resp.headers["X-TTS-Backend"] = FALLBACK
        return resp

    if streaming:
        resp = Response(gen, mimetype="audio/mpeg")
        resp.headers["X-TTS-Cache"] = "miss"
        resp.headers["X-TTS-Stream"] = "1"
        resp.headers["X-TTS-Backend"] = BACKEND
        return resp

    cache_put(key, audio)
    log.info("served %d chars -> %d bytes (%s %s rate=%s pitch=%s) in %.2fs",
             len(text), len(audio), BACKEND, voice, rate, pitch, time.monotonic() - started)
    resp = Response(audio, mimetype="audio/mpeg")
    resp.headers["X-TTS-Cache"] = "miss"
    resp.headers["X-TTS-Backend"] = BACKEND
    return resp


def _fallback_prosody(rate, pitch):
    """Rate is a percentage on both backends; pitch is percent on Azure and Hz
    on edge-tts, so a copy of the Azure default would be rejected there."""
    return rate, ("+0Hz" if FALLBACK == "edge" else DEFAULT_PITCH)


@app.route("/health")
def health():
    return Response(
        json.dumps(
            {
                "service": "muse-tts",
                "backend": BACKEND,
                "fallback": FALLBACK or None,
                "voice": DEFAULT_VOICE,
                "force_voice": FORCE_VOICE or None,
                "style": AZURE_STYLE or None,
                "rate": DEFAULT_RATE,
                "pitch": DEFAULT_PITCH,
                "max_chars": MAX_TEXT_LEN,
                "azure_region": AZURE_REGION,
                "azure_key": "set" if AZURE_KEY else "missing",
                "cache_items": len(_cache),
                "cache_bytes": _cache_bytes,
                "stream_head": STREAM_HEAD_BYTES,
                "send_bytes": SEND_BYTES,
                "test_hooks": TEST_HOOKS,
                "azure_pool_idle": _pool.qsize(),
                "azure_pool_open": _pool_open,
            },
            ensure_ascii=False,
        ),
        mimetype="application/json",
    )


def _prewarm():
    """One tiny synthesis at start-up, so the first real reply does not pay the
    TLS handshake: measured 1.8 s cold against 0.5 s warm."""
    try:
        _synth_azure("嗯", FORCE_VOICE or DEFAULT_VOICE, DEFAULT_RATE, DEFAULT_PITCH)
        log.info("upstream connection warm")
    except Exception as e:  # noqa: BLE001 - a cold first request is not fatal
        log.warning("prewarm failed (first request will be slower): %s", e)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    if BACKEND == "azure" and not AZURE_KEY:
        log.warning("AZURE_SPEECH_KEY is empty: every request will fall back to %s", FALLBACK or "nothing")
    elif BACKEND == "azure":
        threading.Thread(target=_prewarm, daemon=True).start()
    if SERVER == "werkzeug":
        # 0.0.0.0 so the ESP32 on the LAN can reach it.
        app.run(host="0.0.0.0", port=PORT, threaded=True)
    else:
        from waitress import serve
        # send_bytes at the head size so the first audio leaves without waiting
        # for waitress's 1800-byte default to fill.
        log.info("waitress on 0.0.0.0:%d (backend %s, send_bytes %d)", PORT, BACKEND, SEND_BYTES)
        serve(app, host="0.0.0.0", port=PORT, threads=8,
              connection_limit=32, channel_timeout=120, send_bytes=SEND_BYTES)


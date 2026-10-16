#!/usr/bin/env python3
"""
Edge TTS bridge for muse-gadget-sdk firmware (MUSE_TTS_URL).

The firmware does:  GET /tts?voice=<voice>&text=<url-encoded text>
and expects an MP3 back. Long replies are split into ~180-byte chunks
by the firmware, so each request is short.

Install:  pip install flask edge-tts
Run:      python tts_server.py            # listens on 0.0.0.0:5050
Test:     curl "http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD" -o t.mp3

Firmware side (in the board's build sdkconfig, then rebuild + flash):
    CONFIG_MUSE_TTS_URL="http://<this-machine-lan-ip>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
"""

import asyncio
import logging
import os
import re

from flask import Flask, Response, request

import edge_tts

app = Flask(__name__)
log = logging.getLogger("tts")

DEFAULT_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
# Keep in sync with the firmware's per-request chunking; longer single
# requests are slower to start playing.
MAX_TEXT_LEN = 500


def clean(text: str) -> str:
    """Drops what shouldn't be read aloud: code, URLs and Markdown marks."""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links/images: keep text
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[*_`#>|~]+", "", text)
    text = re.sub(r"^\s*[-+]\s+", "", text, flags=re.M)  # list bullets
    return re.sub(r"\s+", " ", text).strip()[:MAX_TEXT_LEN]


async def _synth(text: str, voice: str) -> bytes:
    audio = bytearray()
    async for chunk in edge_tts.Communicate(text, voice).stream():
        if chunk["type"] == "audio":
            audio += chunk["data"]
    return bytes(audio)


@app.route("/tts")
def tts():
    text = request.args.get("text", "")
    voice = request.args.get("voice", DEFAULT_VOICE) or DEFAULT_VOICE
    if not text:
        return "missing text", 400
    text = clean(text)
    if not text:
        return "empty after cleaning", 400
    try:
        audio = asyncio.run(_synth(text, voice))
    except Exception as e:  # noqa: BLE001 - report it, don't crash the server
        log.exception("synthesis failed")
        return f"tts failed: {e}", 502
    if not audio:
        return "empty audio", 502
    log.info("served %d chars -> %d bytes (%s)", len(text), len(audio), voice)
    return Response(audio, mimetype="audio/mpeg")


@app.route("/health")
def health():
    return "ok"


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    # 0.0.0.0 so the ESP32 on the LAN can reach it.
    app.run(host="0.0.0.0", port=5050, threaded=True)

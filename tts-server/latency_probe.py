"""Measure Azure TTS time-to-first-audio-byte under different SSML/format
options. Reads the key from tts-server/.env; never prints it."""
import http.client
import os
import ssl
import sys
import time
from xml.sax.saxutils import escape

ENV = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
vals = {}
with open(ENV, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()

KEY = vals.get("AZURE_SPEECH_KEY", "")
REGION = vals.get("AZURE_SPEECH_REGION", "eastasia")
if not KEY:
    sys.exit("no key")


def ssml(text, voice, rate, pitch, style, degree="1.2", breaks=True):
    parts, buf = [], []
    for ch in text:
        if breaks and ch in "。！？!?；;":
            buf.append(ch)
            parts.append(escape("".join(buf)))
            parts.append("<break time='280ms'/>")
            buf = []
        elif breaks and ch in "，、,":
            buf.append(ch)
            parts.append(escape("".join(buf)))
            parts.append("<break time='120ms'/>")
            buf = []
        else:
            buf.append(ch)
    if buf:
        parts.append(escape("".join(buf)))
    inner = "<prosody rate='%s' pitch='%s'>%s</prosody>" % (rate, pitch, "".join(parts))
    if style:
        inner = ("<mstts:express-as style='%s' styledegree='%s'>%s"
                 "</mstts:express-as>" % (style, degree, inner))
    return ("<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
            "xmlns:mstts='https://www.w3.org/2001/mstts' xml:lang='zh-CN'>"
            "<voice xml:lang='zh-CN' name='%s'>%s</voice></speak>" % (voice, inner))


def one(conn, text, voice="zh-CN-XiaoyiNeural", rate="+8%", pitch="+4%",
        style="friendly", fmt="audio-24khz-48kbitrate-mono-mp3", breaks=True):
    body = ssml(text, voice, rate, pitch, style, breaks=breaks).encode()
    t0 = time.monotonic()
    conn.request("POST", "/cognitiveservices/v1", body, {
        "Ocp-Apim-Subscription-Key": KEY,
        "Content-Type": "application/ssml+xml",
        "X-Microsoft-OutputFormat": fmt,
        "User-Agent": "muse-tts-probe",
    })
    r = conn.getresponse()
    ttfb = None
    n = 0
    while True:
        chunk = r.read(2048)
        if not chunk:
            break
        if ttfb is None:
            ttfb = time.monotonic() - t0
        n += len(chunk)
    return ttfb, time.monotonic() - t0, n


SHORT = "今天天气不错。"
LONG = ("今天的测试编号是七，我们来看看首音时延到底有多少毫秒，这个句子长度大约六十个汉字，"
        "接近一次请求的上限。")

CASES = [
    ("style=friendly  60字 24k/48k", LONG, dict()),
    ("style=none      60字 24k/48k", LONG, dict(style="")),
    ("style=friendly   5字 24k/48k", SHORT, dict()),
    ("style=none       5字 24k/48k", SHORT, dict(style="")),
    ("style=none      60字 16k/32k", LONG, dict(style="", fmt="audio-16khz-32kbitrate-mono-mp3")),
    ("style=none      60字 无break", LONG, dict(style="", breaks=False)),
]

ctx = ssl.create_default_context()
print("%-32s %8s %8s %9s" % ("case", "ttfb_s", "total_s", "bytes"))
for name, text, kw in CASES:
    best = []
    for i in range(3):
        c = http.client.HTTPSConnection("%s.tts.speech.microsoft.com" % REGION,
                                        timeout=25, context=ctx)
        ttfb, total, n = one(c, text + " 第%d次。" % i if len(text) < 10 else text + "第%d次。" % i, **kw)
        c.close()
        best.append((ttfb, total, n))
    best.sort()
    ttfb, total, n = best[len(best) // 2]
    print("%-32s %8.2f %8.2f %9d" % (name, ttfb, total, n))

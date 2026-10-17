import urllib.error
import urllib.request

env = dict(l.split("=", 1) for l in open(".env") if "=" in l and not l.startswith("#"))
key, region = env["AZURE_SPEECH_KEY"].strip(), env["AZURE_SPEECH_REGION"].strip()
URL = "https://%s.tts.speech.microsoft.com/cognitiveservices/v1" % region


def try_ssml(voice, style, text="你好，我是晓伊，这是一次音色与风格测试。"):
    inner = "<prosody rate='+8%' pitch='+4%'>" + text + "</prosody>"
    if style:
        inner = ("<mstts:express-as style='" + style
                 + "' styledegree='1.2'>" + inner + "</mstts:express-as>")
    ssml = ("<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' "
            "xmlns:mstts='https://www.w3.org/2001/mstts' xml:lang='zh-CN'>"
            "<voice xml:lang='zh-CN' name='" + voice + "'>" + inner + "</voice></speak>")
    req = urllib.request.Request(
        URL, data=ssml.encode(), method="POST",
        headers={"Ocp-Apim-Subscription-Key": key, "Content-Type": "application/ssml+xml",
                 "X-Microsoft-OutputFormat": "audio-24khz-48kbitrate-mono-mp3",
                 "User-Agent": "muse-tts-probe"})
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            d = r.read()
            print("%-24s style=%-10s -> HTTP %d, %d bytes, head %s"
                  % (voice, style or "(none)", r.status, len(d), d[:2].hex()))
    except urllib.error.HTTPError as e:
        body = e.read()[:200].decode("utf-8", "replace").replace("\n", " ")
        print("%-24s style=%-10s -> HTTP %d  %s" % (voice, style or "(none)", e.code, body))


for v in ("zh-CN-XiaoyiNeural", "zh-CN-XiaoxiaoNeural"):
    for s in ("friendly", "cheerful", None):
        try_ssml(v, s)

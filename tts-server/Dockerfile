# Azure Speech TTS bridge for muse-gadget-sdk firmware (see tts_server.py).
# Built and run on a LAN host (Alpine 3.17, Docker 20.10).
FROM python:3.12-alpine

WORKDIR /app

# Tsinghua mirror: the host sits behind a CN link, Docker Hub is fine but PyPI is slow.
COPY requirements.txt .
RUN pip install --no-cache-dir \
        -i https://pypi.tuna.tsinghua.edu.cn/simple \
        --trusted-host pypi.tuna.tsinghua.edu.cn \
        -r requirements.txt

COPY tts_server.py .

# No secrets here: the Azure key/region come from /opt/muse-tts/.env on the host,
# passed with `docker run --env-file`.
ENV TTS_VOICE=zh-CN-XiaoxiaoNeural \
    TTS_BACKEND=azure
EXPOSE 5050

# One worker is enough for one board; waitress serves it (threads=8 in
# tts_server.py). The Flask dev server is only for local pokes: it truncated the
# body for the board's HTTP client.
CMD ["python", "tts_server.py"]

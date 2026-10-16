# Edge TTS bridge for muse-gadget-sdk firmware (see tts_server.py).
# Built and run on the LAN host 10.168.2.7 (Alpine 3.17, Docker 20.10).
FROM python:3.12-alpine

WORKDIR /app

# Tsinghua mirror: the host sits behind a CN link, Docker Hub is fine but PyPI is slow.
COPY requirements.txt .
RUN pip install --no-cache-dir \
        -i https://pypi.tuna.tsinghua.edu.cn/simple \
        --trusted-host pypi.tuna.tsinghua.edu.cn \
        -r requirements.txt

COPY tts_server.py .

ENV TTS_VOICE=zh-CN-XiaoxiaoNeural
EXPOSE 5050

# One worker is enough for one board; Flask is threaded so chunks don't queue.
CMD ["python", "tts_server.py"]

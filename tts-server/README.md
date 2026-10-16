# tts-server —— 局域网 edge-tts 播报服务（已部署并验证）

给 ESP32-S3-Touch-LCD-1.85B 提供语音播报：板子把回复文本 GET 过来，服务用 edge-tts 合成 MP3 返回。

**完整计划**：`~/hermes-sync/docs/plan/2026-10-16_ESP32-185B虚拟形象-OTA-TTS计划.md`（阶段 2，任务 T7–T8）

## 部署位置（已验证）

| 项 | 值 |
|---|---|
| 主机 | **10.168.2.7**（Alpine 3.17.0 / x86_64，Docker 20.10.24，**无 compose**） |
| 目录 | `/opt/muse-tts/`（`tts_server.py`、`requirements.txt`、`Dockerfile`，md5 与仓库一致） |
| 镜像 | `muse-tts:1.0`（`python:3.12-alpine` + flask 3.1.3 + edge-tts 7.3.1） |
| 容器 | `muse-tts`，`--restart unless-stopped`，映射 `0.0.0.0:5050->5050` |
| 音色 | 容器环境变量 `TTS_VOICE`，当前 `zh-CN-XiaoxiaoNeural` |

## 为什么自建而不用百度/Google 直连

- 板子在国内网络直连 Google TTS 不通（参考实现 `timmy2048/muse-gadget-185b` 的 `TTS_SKIP_ROOT_CAUSE.txt` 有串口证据）。
- Edge TTS 的 `Sec-MS-GEC` 令牌已是 v2，设备端算不动；**服务端由 `edge-tts` 包维护**，板子只收 MP3。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/tts?voice=<音色>&text=<URL 编码文本>` | 返回 `audio/mpeg`；缺 `text` → 400，合成失败 → 502 |
| GET | `/health` | 返回 `ok` |

## 固件侧接法

在板子的构建目录 sdkconfig 里设（**不入库**），然后重新构建刷入：

```
CONFIG_MUSE_TTS_URL="http://10.168.2.7:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
```

固件会把长回复按标点切成约 180 字节、发起多次 GET（`esp32/components/muse/muse_tts.c`）。
出声前置条件：SETTINGS 里 **Speaker On** 且音量 > 0。

## 部署命令（重装/升级用）

```sh
# 1) 传文件（本地 → 主机，无 sshpass 时用 paramiko 小工具）
python3 ssh_run.py 10.168.2.7 root '<password>' put tts_server.py /opt/muse-tts/tts_server.py
# 2) 构建
ssh 10.168.2.7 'cd /opt/muse-tts && docker build -t muse-tts:1.0 .'
# 3) 运行
ssh 10.168.2.7 'docker rm -f muse-tts; docker run -d --name muse-tts --restart unless-stopped \
  -p 5050:5050 -e TTS_VOICE=zh-CN-XiaoxiaoNeural muse-tts:1.0'
# 4) 验证
ssh 10.168.2.7 'curl -sS "http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD" -o /tmp/t.mp3 && ls -l /tmp/t.mp3'
```

## 实测结果（2026-10-16）

- 主机自测：`GET /tts?...text=你好，我是慕思板子` → 200，14688 字节，4.5 s。
- **从板子同网段（WSL 10.168.2.94）实测**：`GET /health` → 200；`GET /tts?text=测试第N次…` → 200，17568 / 17712 字节，3.9–4.0 s。
- 返回文件开头 `ff f3 64 c4`（MPEG 同步字，**无 ID3 头**），ffprobe 报时长 2.93 s → 合法可播 MP3，minimp3 直接能吃。
- 主机出网：`registry-1.docker.io:443`、`speech.platform.bing.com:443` 均通；宿主**没装 python3**，所以必须走容器。

## 已知限制

- Flask 开发服务器（单进程、threaded），够一块板子用；不上 gunicorn。
- 每个请求要等整段合成完才返回；180 字节分块下延迟约 4 s/块（含 websocket 握手）。嫌慢可改流式返回（参考 `chucky1102/muse-gadget-sdk@local-tts` 的 `server.py`）。
- 无鉴权：局域网内谁都能调。要收口就加 `X-Key`（同参考实现）。
- 响应头尚未带 `X-Emotion`（板载形象的情绪映射要用，见 `../virtual-avatar/README.md`）。
- 磁盘：主机根分区 9.7 G 已用 86%，镜像约 60 MB，留意余量。

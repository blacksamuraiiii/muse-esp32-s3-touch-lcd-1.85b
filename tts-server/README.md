# tts-server —— 局域网 TTS 播报服务（Azure 为主，edge-tts 退居可选回退）

给 ESP32-S3-Touch-LCD-1.85B 提供语音播报：板子把回复文本 GET 过来，服务合成 MP3 返回。
**2026-10-17 起主用 Azure Speech（区域 eastasia，免费 F0 层）**，代码形状照 `timmy2048/muse-gadget-185b`
的 `tools/tts_wg_proxy.py`；edge-tts 那条路保留，可用 `TTS_BACKEND=edge` 切回。

**完整计划**：`~/hermes-sync/docs/plan/2026-10-16_ESP32-185B虚拟形象-OTA-TTS计划.md`（阶段 2，任务 T7–T8）

## 部署位置（已验证）

| 项 | 值 |
|---|---|
| 主机 | 局域网内一台常开 Linux（Alpine 3.17.0 / x86_64，Docker 20.10.24，**无 compose**）；具体地址不入库 |
| 目录 | `/opt/muse-tts/`（`tts_server.py`、`requirements.txt`、`Dockerfile`，md5 与仓库一致） |
| 镜像 | `muse-tts:1.4`（`python:3.12-alpine` + flask + **waitress** + edge-tts；1.0 → 初版，1.1 → 韵律/校验/缓存，1.2 → pitch 单位修正，1.3 → Azure 主用，**1.4 → waitress 换掉 Flask 开发服务器**） |
| 容器 | `muse-tts`，`--restart unless-stopped`，映射 `0.0.0.0:5050->5050`，`--env-file /opt/muse-tts/.env` |
| 密钥 | 主机 `/opt/muse-tts/.env`（mode 600，不入库、不进镜像）；本地副本 `tts-server/.env`（已 gitignore） |
| 音色 | `TTS_VOICE`，当前 `zh-CN-XiaoxiaoNeural`；Azure 侧再叠 `mstts:express-as style=friendly styledegree=1.2` |

## 为什么自建而不用百度/Google 直连

- 板子在国内网络直连 Google TTS 不通（参考实现 `timmy2048/muse-gadget-185b` 的 `TTS_SKIP_ROOT_CAUSE.txt` 有串口证据）。
- Edge TTS 的 `Sec-MS-GEC` 令牌已是 v2，设备端算不动；**服务端由 `edge-tts` 包维护**，板子只收 MP3。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/tts?voice=<音色>&text=<URL 编码文本>` | 返回 `audio/mpeg`；`q=` 与 `text=` 都收；缺文本 → 400，上游非 MP3 → 502 |
| GET | `/health` | JSON：生效音色、rate、pitch、max_chars、缓存条目与字节数 |

可选参数：`rate`（默认 `+8%`）、`pitch`（默认 `+4%`）、`stream`（默认 `1`）。**pitch 单位按后端不同**：
Azure 收百分比（`^[+-]\d+%$`），edge-tts 只收 `^[+-]\d+Hz$`，写错会被忽略并回落默认值，
不会打断这一轮朗读（这是直接照搬 timmy 的 Azure 版 `+4%` 到 edge-tts 时踩过的坑）。

## Azure Speech 为主（镜像 1.3，2026-10-17）

调用形状照 `timmy2048/muse-gadget-185b` 的 `tools/tts_wg_proxy.py`：

- `POST https://<region>.tts.speech.microsoft.com/cognitiveservices/v1`，
  头 `Ocp-Apim-Subscription-Key` / `Content-Type: application/ssml+xml` / `X-Microsoft-OutputFormat`
- SSML：`<mstts:express-as style='friendly' styledegree='1.2'>` 包 `<prosody rate='+8%' pitch='+4%'>`，
  句末 `。！？!?；;` 插 `<break time='280ms'/>`、逗号类插 `<break time='120ms'/>`
- 输出格式默认 `audio-24khz-48kbitrate-mono-mp3`——与板子已经在放的一致（串口 `reply audio: 24000 Hz, 1 ch, 48 kbps`）
- 后端开关：`TTS_BACKEND=azure|edge`（默认 azure）；`TTS_FALLBACK` 可让首选后端「一个字节都没出」时换另一条路重试

WSL 本机实测（`tts-server/.env` 提供 key/region，`~/.venv/bin/python3 tts_server.py`）：

| 场景 | 首字节 | 全部完成 | 字节数 | 校验 |
|---|---|---|---|---|
| 流式，35 字（6.79 s 音频） | **1.76–2.81 s** | 2.20–3.47 s | 40,752 | ffprobe `mp3 24000 Hz mono 48 kbps` |
| 同句二次请求 | 0.003 s | 0.003 s | 40,752 | LRU 命中，md5 一致 |
| `stream=0` 缓冲，25 字 | 2.69 s | 2.69 s | 30,384 | 5.06 s 音频，合法 MP3 |
| 154 字整句 | 2.20 s | 3.27 s | 93,888 | 15.65 s 音频 |

对比旧 edge-tts：首字节量级相当（1.29 s 起，见下节 A/B），但**edge-tts 现已不稳定**——
本机实测 WSS 握手 403（`Sec-MS-GEC` 令牌/版本漂移），所以默认不再挂它做回退
（挂上只会让失败路径多等约 7 秒）。要试就 `TTS_FALLBACK=edge`。

部署到 <TTS 主机> 后的验证（2026-10-17，镜像 muse-tts:1.3）：

| 位置 | 结果 |
|---|---|
| 主机自测 | `/health` → `backend=azure, key=set`；`/tts` → 200，首字节 2.21 s，14832 B，头 `ff f3 64 c4` |
| 同网段 WSL 实测（板子走的就是这条） | `/tts` → 200，首字节 3.54 s，总 3.82 s，42480 B，ffprobe `mp3/24000/mono/48kbps/7.08 s` |

主机首字节比 WSL 本机跑（2.2 s）略高，差在主机到 Azure eastasia 的链路；整段仍比旧的
edge-tts 路径（约 4 s/块）快或持平，且不再依赖微软的令牌握手。

### 流式返回（v1.3，首音提速）

默认 `stream=1`：边合成边用 `Transfer-Encoding: chunked` 推送，先攒够 1024 字节确认是 MP3 再发 200，
之后来一块发一块；整段结束仍写入缓存。`stream=0` 回到「合成完再发」的旧行为。

实测 A/B（同一段 170 字中文 ≈ 20.6 秒音频，各自冷启动，容器重启清缓存）：

| 模式 | 首字节 | 全部完成 | 字节数 |
|---|---|---|---|
| `stream=0` 缓冲 | 3.15 s | 3.15 s | 123552 |
| `stream=1` 流式 | **1.29 s** | 3.05 s | 123552 |

首字节提前 **1.86 s（降 59%）**；两份字节数一致，取回后 `ffprobe` 校验 `mp3 / 24000 Hz / mono / 20.59 s / 48 kbps`，内容完整。
固件侧同步加了度量：串口 `muse.tts` 行现在会打 `content-length`、`first byte N ms`、`total N ms`——
流式响应没有 Content-Length（值为 -1），所以字节数改由固件自己累计。

**注意端到端的瓶颈不在这一环**：Muse 服务端把回复文本吐给板子要 4–7.6 秒，本机合成只占约 2–3 秒。

**v1.2 相对你初版新增**（参考 `timmy2048/muse-gadget-185b` 的 `tools/tts_wg_proxy.py`）：

- 韵律参数 `rate`/`pitch`（他的 Azure 版用 `+8%`/`+4%`；`style=express-as` 是 Azure 专属，edge-tts 没有）
- **MP3 合法性校验**：`len>64` 且首字节 `0xFF` 或前 3 字节 `ID3`，否则返 502——避免上游错误正文被当音频喂给解码器
- `MAX_TEXT_LEN` 500 → **180**，与固件 180 字节分块对齐，首音更快
- `/health` 由纯文本 `ok` 改为 JSON
- 参数名兼容 `q`
- 另加**结果 LRU 缓存**（同句重复回复直接命中；实测 4.13 s → 0.0033 s）

## 实测结果（2026-10-16）

- 主机自测：`GET /tts?...text=你好，我是慕思板子` → 200，13680 字节，4.13 s；同句二次请求 → 200，**0.0033 s**，`X-TTS-Cache: hit`
- **从板子同网段（WSL <同网段主机>）实测**：`GET /health` → 200；`GET /tts?text=测试第N次…` → 200，17.5 KB，3.9–4.0 s
- 返回文件开头 `ff f3 64 c4`（MPEG 同步字，**无 ID3 头**），ffprobe 报时长 2.93 s → 合法可播 MP3，minimp3 直接能吃
- **真机端到端出声已验证**（2026-10-16）：板子三轮全通，串口出现 `TTS got 29664/36144/48240 bytes` 与
  `reply audio: 24000 Hz, 1 ch, 48 kbps`；edge-tts 固定 24 kHz，固件重采样到 16 kHz 输出。
  时序样例（秒，自松手算）：`text +4.0 → done +4.9 → mp3 +7.8 → audio +7.8`
- 主机出网：`registry-1.docker.io:443`、`speech.platform.bing.com:443` 均通；宿主**没装 python3**，所以必须走容器

## 固件侧接法

在板子的构建目录 sdkconfig 里设（**不入库**），然后重新构建刷入：

```
CONFIG_MUSE_TTS_URL="http://<TTS 主机>:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}"
```

固件会把长回复按标点切成约 180 字节、发起多次 GET（`esp32/components/muse/muse_tts.c`）。
出声前置条件：SETTINGS 里 **Speaker On** 且音量 > 0。

## 部署命令（重装/升级用）

已封装成 `deploy.py`（paramiko，直连主机）：

```sh
cd tts-server
MUSE_HOST_PASSWORD='<root 口令>' ~/.venv/bin/python3 deploy.py <TTS 主机>
```

它做四件事：上传 `tts_server.py`/`requirements.txt`/`Dockerfile` → 在主机写 `/opt/muse-tts/.env`（mode 600，
只含 `AZURE_SPEECH_KEY`/`AZURE_SPEECH_REGION`/`TTS_VOICE` 等白名单键）→ `docker build -t muse-tts:1.3 .`
→ `docker rm -f muse-tts` + `docker run -d --env-file /opt/muse-tts/.env -p 5050:5050 muse-tts:1.3`，
最后在主机上 curl `/health` 与一条 `/tts` 自证。口令从环境变量读，不进 argv / 历史记录。

手工等价命令：

```sh
ssh root@<TTS 主机> 'cd /opt/muse-tts && docker build -t muse-tts:1.3 .'
ssh root@<TTS 主机> 'docker rm -f muse-tts; docker run -d --name muse-tts --restart unless-stopped \
  --env-file /opt/muse-tts/.env -p 5050:5050 muse-tts:1.3'
ssh root@<TTS 主机> 'curl -sS "http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD" -o /tmp/t.mp3 && ls -l /tmp/t.mp3'
```

## 镜像 1.4（2026-10-17 晚）：换掉 Flask 开发服务器（板子端真凶）

**症状**：Azure 上线后板子完全没声音。串口：

    E HTTP_CLIENT: Incomplete chunked data received 0
    W muse.tts: TTS request failed: ESP_ERR_HTTP_INCOMPLETE_DATA
    E HTTP_CLIENT: Incomlete data received, ret=0, 1440/51264 bytes   ← 只收到 1440 字节

**排除法（每步都是实测）**：

| 步骤 | 结果 | 结论 |
|---|---|---|
| 容器日志 | `streamed 51264 bytes in 4.23s`，Azure 侧完整 | 服务端没少发 |
| WSL 用同一 URL 取 | 200，收满 51467 B（含头） | 载荷与线路没问题 |
| **裸 socket 探针**占 5050 端口（同一容器镜像、同一网络、同一 51264 B 载荷，绕开 werkzeug） | **板子完整播报** | 问题在 Flask **开发服务器/werkzeug** 这一层 |
| 换 waitress 后（镜像 1.4） | 板子 `TTS got 13248 bytes`，正常出声 | 修复有效 |

**处置**：`TTS_SERVER` 默认 `waitress`（`send_bytes=1024`、`threads=8`、`channel_timeout=120`），
Flask 开发服务器只留本地 `curl` 用（`TTS_SERVER=werkzeug`）。requirements 加 `waitress>=3.0`。

**留下可复用的诊断手法**：怀疑"服务端发不全"时，用一个裸 socket 服务占住同一端口、
回放**同一份**已知会失败的字节流；板子若能放完，责任立刻落到 HTTP 服务器/代理层。
（`~/.hermes/cache/scratch/probe_server.py` 就是这个探针。）

蹲坑记录：`Connection: close` + 开发服务器的组合会让板子只拿到约两个 TCP 段（1440 B）就 EOF；
chunked 与 Content-Length 两条路都一样，所以不是分块编码的问题。

## 已知限制

- **waitress** 服务（threads=8）；Flask 开发服务器只用于本地 curl（`TTS_SERVER=werkzeug`）——
  它会把板子的响应截断（详见上一节），别再拿它上生产。
- **Azure 免费层 F0**：约 50 万字符/月；超了会 401/429 → 服务返 502，板子按「非 MP3」丢弃、只留文字
  （板子侧闸门见 `esp32/components/muse/muse_tts.c`）。要续就换 key 或升级层。
- edge-tts 路已不可靠：本机实测 WSS 403（`Sec-MS-GEC` 漂移），`TTS_BACKEND=edge` 仅作应急。
- 180 字节分块下每块都是一次独立请求（Azure 侧实测首字节 1.8–2.8 s）；嫌慢只能减小分块或换更快的层。
- 无鉴权：局域网内谁都能调。要收口就加 `X-Key`（同参考实现）。
- 响应头尚未带 `X-Emotion`（板载形象的情绪映射要用，见 `../virtual-avatar/README.md`）。
- 磁盘：主机根分区 9.7 G 已用 86%，镜像约 60 MB，留意余量。

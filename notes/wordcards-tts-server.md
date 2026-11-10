# 单词卡片发音：服务端实现与预生成

> 2026-11-10 落地。服务端代码在仓库 `tts-server/`（`tts_server.py` 的 `word=1` 分支
> + `gen_word_audio.py`），已部署到跑 `muse-tts` 的那台主机（地址不入库）。
> 固件侧见 `feat/wordcards-20261110`：进单词卡片时后台预取 4 个词的 MP3 到 PSRAM，
> 翻面本地播放，**翻面不调 Azure**。

## 一句话机制

固件把回复 TTS 的同一个 `/tts` 地址拿来取单词，只多带一个 `word=1`：

    固件模板： https://<TTS 主机>/?k=<token>&voice=zh-CN-XiaoyiNeural&text={text}
    单词请求： …&text=apple&word=1

服务端看到 `word=1` 就**忽略 URL 里的 voice/rate**，固定用单词语音
`en-US-AriaNeural`、语速 `-10%`，并优先读磁盘上的预生成文件；命中直接返回，
未命中才合成一次并顺手存下。中文回复音色永远不会读英文单词；普通回复请求
（无 `word=1`）走原来的逻辑、原来的音色。

文件名 = `词-sha256(voice|rate|词)前12位.mp3`，所以换音色或换语速不会误用旧文件。

## 服务端改了什么（`tts-server/tts_server.py`，1.5 → 1.6）

- `word=1` 分支（`_word_reply`）：磁盘命中 → 返回文件；未命中 → Azure 合成
  `en-US-AriaNeural -10%` → 原子写入（`.tmp` + `os.replace`）→ 返回。
- 磁盘缓存：`TTS_WORD_DIR`（默认 `/app/word_audio`）；`TTS_WORD_MAX_FILES`
  默认 2000，到顶后只读不写、不删旧文件；非 MP3 的文件按缺失处理并留给人看。
- SSML 语言跟随音色：`en-*` 用 `en-US`，其余 `zh-CN`（原来写死 `zh-CN`，
  英文词用英文音色会读错）。
- `/health` 增 `word_voice` / `word_rate` / `word_dir` / `word_files` / `word_max_bytes`。
- `gen_word_audio.py`：按固件词表一次一词预生成，已在且校验通过就跳过，重跑安全。
- `Dockerfile`：`COPY gen_word_audio.py` + `mkdir -p /app/word_audio`。

可调环境变量：`TTS_WORD_VOICE` / `TTS_WORD_RATE` / `TTS_WORD_PITCH` /
`TTS_WORD_DIR` / `TTS_WORD_MAX_FILES` / `TTS_WORD_MAX_BYTES`（默认 32768，与固件
`MUSE_WORDCARDS_MAX_MP3_BYTES` 对齐）。

## 部署现状（已做）

| 项 | 值 |
|---|---|
| 镜像 | `muse-tts:1.6`（`muse-tts:1.5` 保留，回滚直接 `docker run` 1.5） |
| 容器 | `muse-tts`，`--env-file /opt/muse-tts/.env`，`-p 127.0.0.1:5050:5050` |
| 挂载 | `-v /opt/muse-tts/word_audio:/app/word_audio`（重建镜像不丢词音） |
| 旧文件 | `/opt/muse-tts/tts_server.py.1.5` 留档 |
| 词音 | 32 个词全部预生成，共 388 KB |

重新部署（改代码后）：

```sh
# 本机
scp tts-server/tts_server.py tts-server/gen_word_audio.py tts-server/Dockerfile \
    root@<TTS 主机>:/opt/muse-tts/
# 主机
cd /opt/muse-tts && docker build -t muse-tts:1.6 . && \
  docker rm -f muse-tts && \
  docker run -d --name muse-tts --restart unless-stopped \
    --env-file /opt/muse-tts/.env -p 127.0.0.1:5050:5050 \
    -v /opt/muse-tts/word_audio:/app/word_audio muse-tts:1.6
docker exec muse-tts python gen_word_audio.py     # 缺的词补上，已有的跳过
```

## 验证记录（2026-11-10，实测）

| 位置 | 检查 | 结果 |
|---|---|---|
| 本机 WSL（5051 端口） | 预生成文件 + `word=1` | 200 `X-TTS-Cache: hit`，字节与磁盘文件 md5 一致 |
| 本机 WSL | URL 带 `voice=zh-CN-XiaoyiNeural` 仍取单词音 | md5 不变（说明忽略回复音色） |
| 本机 WSL | 普通中文回复 | 200 `X-TTS-Stream: 1`，MP3 合法（未受影响） |
| 主机容器内 | `docker exec … gen_word_audio.py` | `done: 32 generated, 0 already cached, 0 failed` |
| 主机 127.0.0.1:5050 | `word=1` 两次 + 表外词 `table` | hit / hit / miss 后自动存盘，均 200 |
| 公网 URL（WSL 按固件模板形状） | `…&text=apple&word=1` | HTTP/2 200、`x-tts-cache: hit`、`x-tts-word: 1`，ffprobe `mp3/24000/mono/48kbps/1.872s` |

## 取音提速（2026-11-10，1.5.2c）

固件原来每词一次 `esp_http_client_init` → `perform` → `cleanup`，即每词新建一条
TCP+TLS 连接。板子上单次握手 3.5–4.3 秒（服务器同一连接应答只要 18 ms），所以一组
4 个词要 16 秒（串口时间戳：apple 123.29 s → banana 128.26 s → cherry 173.24 s →
peach 176.71 s）。现改为**一条 keep-alive 连接串行取完**（只换 URL，出错/被中止时才
丢弃重连），一组降到数秒，用户实机确认「快多了」。

nginx 侧同时加了 upstream keep-alive（`proxy_set_header Connection ""` +
`vhost/nginx/0.muse-tts-upstream.conf` 的 `upstream muse_tts_backend { keepalive 16; }`），
省掉 nginx→容器每请求新建连接。

## 已知观察

- **每个词音 11232 字节（1.872 s），其中语音只占约 0.5 s**，其余是 Azure 尾部静音；
  中文短回复（如「你好」）同样是 11232 字节，属既有输出格式行为，不是这次改动引入。
  若嫌翻面后静音拖太久，可在服务端生成时裁掉尾部静音（`ffmpeg silenceremove` 或
  逐帧裁剪），目前**未做**——固件侧不限时长，不影响正确性。
- 词表：水果/动物/颜色/身体/数字/学校/食物/家庭，共 32 词，与
  `esp32/components/muse/muse_wordcards.c` 的 `MUSE_WORDCARDS_SETS_TABLE` 同款。
  改词表时**两处都要改**（固件表 + `gen_word_audio.py` 的 `WORDS`），再跑一次生成。
- 目前只有水果、动物两组有图可玩；其余 6 组的词音已备好，等美术到位即可用。

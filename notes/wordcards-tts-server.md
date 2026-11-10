# 单词卡片发音：主机侧步骤（TTS 服务器）

> 用户在自己主机上跑的步骤，助手不碰部署、不碰 key。
> 固件（`feat/wordcards-20261110`）已改好：进单词卡片时后台预取 4 个词的 MP3
> 到 PSRAM，翻面立即本地播放；**翻面时不调 Azure**，发音全部来自这台服务器上
> 预生成的静态文件。`key` 只在主机 `.env` 里，绝不进代码、不进聊天。

## 先确认一件事：固件的语音 host 是不是这台桥

固件的 TTS 模板（设置里的语音服务地址）指向哪台主机，单词文件就必须放在哪台。
`CONFIG_MUSE_TTS_URL` 在公开构建里是空的，地址存在设备的 NVS 设置里——
在设备上看一眼：设置 → 语音（Speech）那一行显示的 host。

- 如果是桥主机（现在跑 `muse-tts:1.4` 的那台，`<桥主机内网地址>:5050`）：按下面做。
- 如果指向的是 relay 或别的 host：文件和步骤完全一样，只是换一台机器跑——
  把 `tts_server_azure.py` + `gen_word_audio.py` 放到模板指向的那台主机上。

## 改了什么（服务器侧）

文件在 `~/workspace/wordcards-tts-server/`，都是准备好的、没部署的：

- `tts_server_azure.py` —— 现有桥的更新版（在 `~/workspace/your_files/muse-tts-edge-bridge/`
  那份基础上改的，回复逻辑没动）：
  - `/tts` 加磁盘缓存（`WORD_AUDIO_DIR`，容器里默认 `/app/word_audio`）：
    缓存键是 voice + rate + text 的 SHA-256，命中直接返回文件、不调 Azure；
    未命中才合成并顺手存入。缓存超 `CACHE_MAX_FILES`（默认 2000 个文件）时只
    读不写，不删旧文件。
  - `word=1` 请求（固件 `muse_tts_url_for_text` 自动带上）：忽略 URL 里的
    voice/rate，固定用单词语音 `en-US-AriaNeural`、语速 `-10%`（`WORD_VOICE` /
    `WORD_RATE` 环境变量可改）。**中文回复音色永远不会读英文单词**；普通回复
    请求（没有 `word=1`）走原来的逻辑、原来的音色，一字没动。
  - `_synth` 的 SSML 语言跟随音色：`en-` 开头的音色用 `en-US`，否则 `zh-CN`
    （原来写死 `zh-CN`，英文词用英文音色时会读错）。
- `gen_word_audio.py` —— 预生成脚本：调 Azure 一次一词，把 32 个词
  （水果/动物/颜色/身体/数字/学校/食物/家庭，固件词表同款）写进缓存目录。
  已在缓存里且 MP3 校验通过的词会跳过，重跑安全。只生成、不部署。

## 步骤（主机上，`<桥主机内网地址>` 的 `/opt/muse-tts/`）

```sh
cd /opt/muse-tts/
# 1. 把两个新文件拷进去（从助手这台机器拿过去）
#    tts_server_azure.py、gen_word_audio.py
#    （如果 /opt/muse-tts/ 已有 Dockerfile.azure，直接用它重建）

# 2. 先看清现在容器是怎么跑的（key 和端口要原样保留）
docker ps --format '{{.Image}} {{.Ports}} {{.Names}}'
docker inspect <容器名> --format '{{json .Config.Env}}'
#    记下：镜像名（muse-tts:1.4）、-e AZURE_SPEECH_KEY、-p 5050:5050、
#    以及 AZURE_SPEECH_REGION / TTS_VOICE 这些 -e

# 3. 重建镜像并重起容器（换新版桥文件；key 从 .env 传，不进镜像）
docker build -f Dockerfile.azure -t muse-tts:1.5 .
docker stop <容器名> && docker rm <容器名>
docker run -d --name <容器名> --restart unless-stopped \
  -p 5050:5050 \
  -e AZURE_SPEECH_KEY="$(grep AZURE_SPEECH_KEY /opt/muse-tts/.env | cut -d= -f2-)" \
  -e AZURE_SPEECH_REGION=eastasia \
  -e TTS_VOICE=zh-CN-XiaoxiaoNeural \
  muse-tts:1.5
#    （TTS_VOICE / REGION 按第 2 步看到的实际值填；上面只是示例）

# 4. 在容器里预生成 32 个词（key 已在容器环境里，不用再传）
docker exec <容器名> python gen_word_audio.py
#    期望输出：每词一行 "apple: 12345 bytes -> word_audio/....mp3"，
#    最后 "done: 32 generated, 0 already cached, 0 failed"
#    （已生成过的词重跑会显示 already cached）

# 5. 验证：单词请求走缓存、英文音色
curl "http://127.0.0.1:5050/tts?word=1&text=apple" -o /tmp/apple.mp3
file /tmp/apple.mp3        # 应为 MPEG audio（不是 HTML/报错）
#    再看容器日志，应有一行 cache hit：
docker logs <容器名> | grep -i "cache hit" | tail -3
#    第二次 curl 同一个词必须也是 cache hit（不再调 Azure）

# 6. 验证：普通回复没变（中文、无 word=1）
curl "http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD" -o /tmp/hi.mp3
file /tmp/hi.mp3          # 应为 MPEG audio
```

## 注意事项

- **缓存目录在容器里**（`/app/word_audio`）：`docker restart` 不丢文件；
  但下次重建镜像会丢——重建后重跑一次第 4 步（一个命令，跳过已生成的）。
  想彻底不丢就给容器加 `-v /opt/muse-tts/word_audio:/app/word_audio`，
  那是可选的，不做也不影响使用。
- 32 个词每个约 5–20KB，总共几百 KB；Azure Free F0 额度足够（一次一词，
  只在预生成时调 32 次，之后翻面零调用）。
- 固件侧 32KB/词上限：如果某个词的 MP3 超过 32KB，固件会把它判为失败、
  那张卡片静默（只显示文字）。`gen_word_audio.py` 输出里每个词都有字节数，
  超了会看到——正常英文单词不会超。
- 如果第 5 步 `curl` 返回的不是 MP3（`file` 显示 HTML 或 text）：
  先看 `docker logs` 里的报错（最常见是 key 没传进去，`/tts` 会报 500
  `AZURE_SPEECH_KEY not set`），不要动固件。
- 回复 TTS 的行为没变：同一份文件、同一个 `/tts`、同一把 key；
  只是多了缓存和 `word=1` 分支。改之前跑着的语音回复，改之后照常跑。

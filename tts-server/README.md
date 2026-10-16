# tts-server —— 局域网 edge-tts 服务（规划中，未开工）

给 ESP32-S3-Touch-LCD-1.85B 提供语音播报：板子把回复文本发过来，服务返回 MP3 流。
部署在 **10.168.2.7**（Alpine + Docker），与板子（`10.168.2.85`）同网段。

**完整计划**：`~/hermes-sync/docs/plan/2026-10-16_ESP32-185B虚拟形象-OTA-TTS计划.md`（阶段 2，任务 T7–T8）

## 为什么自建而不用百度/Google 直连

- 板子在国内网络直连 Google TTS 不通（参考实现 `timmy2048/muse-gadget-185b` 的 `TTS_SKIP_ROOT_CAUSE.txt` 有串口证据）。
- Edge TTS 的 `Sec-MS-GEC` 令牌已是 v2，设备端算不动；**服务端由 `edge-tts` 包维护**，板子只收 MP3。

## 接口约定（待实现）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/tts?voice=zh-CN-XiaoxiaoNeural&text={text}` | 贴合固件已有的 GET 模板（`CONFIG_MUSE_TTS_URL`），改一行 URL 即切换 |
| POST | `/tts` | 备选：整段文本 + `X-Key` 鉴权 + 流式返回（参考 `chucky1102/muse-gadget-sdk@local-tts`） |
| GET | `/healthz` | 健康检查 |

响应头预留 `X-Emotion`（情绪标签，供板载形象用；见 `../virtual-avatar/README.md`）。

## 关键事实

- 固件侧通路已存在：`esp32/components/muse/muse_tts.c`（GET 模板、长回复按标点切 180 字节分多次请求）。
- 出声前置条件：SETTINGS 里 Speaker On 且音量 > 0。
- 端口与鉴权待定，默认拟用 8020。

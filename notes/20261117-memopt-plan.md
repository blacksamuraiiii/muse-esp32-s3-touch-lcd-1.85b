# 1.5.4 内存优化设计（开发版）

> 基点：`main` 头 `803ca78`（1.5.3 正式版 + README 状态行一笔），分支 `feat/memopt-20261117`，工作克隆 `~/workspace/muse-memopt-20261117`。
> 日期：2026-11-17。性质：**只读调研 + 设计文档，不改代码、不提交**。
> 目标：① 给以后可能上的 Hey Muse 唤醒词腾空间；② 治单词/听力 TTS 首音慢与 Muse 连接页 OOM。
> 口径：所有“省多少”分两栏——**代码可算**（从代码直接算出的字节数）与**须实机量测**（只能实测的数）。推导数不写成实测数。
> 约束：不动 `WC_MIN_INTERNAL_FOR_TLS 4608`（只当验收观测点）；不动 `esp32/version.txt`、`ota/latest.json`、`.github/workflows/*`。

## 0. 结论先行

- 瓶颈是**内部 SRAM（尤其 DMA-capable 池）**，不是 8 MB PSRAM。最近一次实测（1.5.3b，`be303d0`）内部堆在词卡进页那一刻是 **27K 空闲 / 7K 最大块**——7K 连一个 8 KB 的 `muse_vm` 任务栈都放不下，这正是上游 `d502649` / `7b302e3` 修的那类故障在本板上的现状。
- Flash 账：bin 4,067,328 B，4M app 槽剩 **126,976 B**；唤醒词上游实测 +148 KB，缺口约 **21,024–24,576 B**（取决于 148 KB 按 1000 还是 1024 算）。
- 本板已经做过一轮大的内存优化（§1.3），调研报告里的大项多数**已经落地或已被本地代码明确拒绝**，不要重复做。本设计只列核实过能做的 8 项（R1–R7 内部 RAM、F1 flash），每项可单项回退。
- 常驻内部 RAM 可算节省（R4+R5+R6+R7）：**18,080 B**；峰值另加 R1（播放时 12,288 B）与 R2（setup/lookup 时 20,480 B）——峰值不等于稳态空闲，见 §2 末尾的合计口径。R3 量级须量测。Flash：F1 离线模型估算全量省约 269 KB（仅词卡约 187 KB），须实测编解码后定。

## 1. 现状基线

### 1.1 Flash 账（代码可算）

| 项 | 数字 | 来源 |
|---|---|---|
| app 槽 | 4,194,304 B（4M） | `esp32/partitions_muse.csv`：`ota_0, app, ota_0, 0x20000, 4M` |
| 1.5.3 bin | 4,067,328 B | `ota/latest.json`：`"size": 4067328`（sha256 `2ee37963…`） |
| 剩余 | **126,976 B** | 4,194,304 − 4,067,328 |
| 唤醒词增量 | +148 KB（上游实测） | 上游 `ea93659`（2026-11-17）：“app image grows by ~148 KB (Watcher, BOX-3, StopWatch)” |
| 缺口 | **21,024 B**（148,000 计）或 **24,576 B**（151,552 = 148 KiB 计） | 算术推导，非实测 |
| 唤醒词运行时 RAM | runner 29 KB PSRAM + 14 KB 内部；音频缓冲 ~133 KB PSRAM；静态内部 +1.6 KB | 上游 `ea93659` 实测（SenseCAP Watcher） |

即：**flash 至少要再省约 25 KB 才装得下唤醒词**（取严的 24,576 B + 余量）；运行时内部 RAM 至少要给 runner 留约 14 KB，同时最大块不能低于 8 KB（`muse_vm` 任务）。

### 1.2 内部 RAM 基线（实测数，带出处）

| 时刻 / 指标 | 数字 | 来源 |
|---|---|---|
| 1.5.3b 词卡进页那一刻，内部空闲 / 最大块 | **27K / 7K**（改前 18K/4K） | `be303d0` 提交说明：Wi-Fi 静态 RX 8→4、BA 窗口 16→8 后“内部堆 18K/4K → 27K/7K，实测刷机后恒定、无 Guru/assert” |
| 同一时刻的门槛对照 | 最大块 5 KB 左右时，6 KB 预取门仍会全拒，故门槛 6 KB→4608 B | `d271838` 提交说明 |
| 失败日志（8 KB 任务起不来） | `muse_vm task create failed: internal free %u largest %u` | `esp32/main/app.c:2570-2572`；更早的实机记录见板文件注释：`internal free 8931 largest 5376`（`board_waveshare_s3_touch_lcd_185b.c:1250-1256` 附近注释） |

**打点基础设施已存在，不用新写**：心跳每轮打印分池三组数（`esp32/main/app.c:3136-3154`）：

```c
size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
size_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);  // 启动以来低水位
size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
// 日志格式：hb ... int=%uK/%uK min=%uK dma=%uK/%uK psram=%uK
```

另有逐任务栈水位：`stack_monitor_record(NULL)`（`esp32/main/stack_monitor.h`，`minimum_free_bytes`），`app.c` 各任务结尾已调用。词卡预取门日志：`prefetch skipped: internal %u B free, largest %u B (floor %u)`（`esp32/components/muse/muse_wordcards_audio.c:377`）。

### 1.3 已经做过的（不要重复做，逐项核实）

| 已做项 | 位置 | 说明 |
|---|---|---|
| mbedTLS 全量进 PSRAM | `esp32/sdkconfig.defaults:136-140` | `MBEDTLS_EXTERNAL_MEM_ALLOC=y`、`MBEDTLS_INTERNAL_MEM_ALLOC=n`；IN/OUT 各 16,384 B（`:131-132`）走 PSRAM。注释写明 `DYNAMIC_BUFFER=n` 是故意的：“its heap path doesn't honor EXTERNAL_MEM_ALLOC, so it fragmented internal SRAM” |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` 16384→4096 | `esp32/sdkconfig.defaults:57` → `esp32/devices/sdkconfig.muse:35` | ≥4 KB 的 malloc 先走 PSRAM |
| 内部保留池 64 KB | `esp32/sdkconfig.defaults:62` | `SPIRAM_MALLOC_RESERVE_INTERNAL=65536`，只供显式内部/DMA 分配 |
| Wi-Fi 静态 RX 8→4、BA 16→8 | `esp32/devices/sdkconfig.muse:76-90`（含注释） | 即 §1.2 的 18K/4K → 27K/7K |
| Wi-Fi 静态 TX 16→8 | `esp32/devices/sdkconfig.muse-waveshare-s3-touch-lcd-185b` | “8, not 16, at 1.6 KB each, which voice over the tunnel doesn't fill” |
| Wi-Fi/lwIP 动态缓冲走 PSRAM | 同上 overlay：`SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`；`sdkconfig.muse` 同 |
| I2S 环 6×240→6×120 | `board_waveshare_s3_touch_lcd_185b.c:1246-1256` | 默认 17.3 KB（tx 6×960 B + rx 6×1920 B）→ 8.6 KB，每方向 45 ms 余量 |
| LVGL 绘图走 PSRAM bands + 内部小 chunk | `board_waveshare_s3_touch_lcd_185b.c:206-217`、`muse_lcd_bands.c:394-407` | 4 bands × 90 行（`DRAW_BUF_LINES = 360/4`），`use_psram=true`，双缓冲共 129,600 B 在 PSRAM；发送只用两个内部 chunk 各 2,880 B（4 行） |
| LVGL 对象/样式走 PSRAM | `esp32/components/muse/muse_lv_mem.c` | `lv_malloc_core` 用 `MALLOC_CAP_SPIRAM`，`CONFIG_LV_USE_CUSTOM_MALLOC=y`（`sdkconfig.muse`）——`LV_MEM_SIZE` 不适用 |
| 会话 turn（~10 KB）进 PSRAM | `esp32/components/muse/muse_chat_session.cpp:270` | `EXT_RAM_BSS_ATTR static turn_t s_turn`；overlay 已开 `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`，故本板上它确实在 PSRAM |
| 会话缓冲/队列/流全进 PSRAM | `muse_chat_session.cpp:312, 2055, 2411-2430` | `psram_alloc`、`MALLOC_CAP_SPIRAM` 队列与流缓冲；`muse_chat` 任务栈 48 KB 在 PSRAM（`:2448`） |
| TTS/词卡任务栈进 PSRAM | `muse_tts.c:909-911`（prewarm 8192）、`:935`（tts 12288）、`:1208`（probe 10240）；`muse_wordcards_audio.c:476-478`（fetch 12288）、`:846-848`（play 24576） | 全部 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` |
| TTS ring/解码缓冲进 PSRAM | `muse_tts.c:924`（16,384 B ring）；`muse_wordcards_audio.c:194, 210-213`（每词 32 KB 槽、`s_dec/s_pcm/s_pcm16`） | 词卡每词槽 32 KB（`MUSE_WORDCARDS_MAX_MP3_BYTES 32768`，`muse_wordcards.h:84`） |
| 语音 pre-roll/录音缓冲进 PSRAM | `muse_voice.c:1029`（`s_rec`）、`:1171`（`s_pre`，`MUSE_BIG_CAPS`） | 只有 `s_chunk` 640 B 还在内部（见 R5） |
| 音效/chirp 缓冲进 PSRAM | `muse_audio.c:575-580`（chirp）、`:611-617`（SFX）；SFX 任务栈 PSRAM（`:673`，`MUSE_BIG_CAPS`） | chirp 注释明确写了“2.9 KB 内部块买不到什么” |
| TTS keep-alive + 预热 | `muse_tts.c:353`（`keep_alive_enable=true`）、`:873`（`muse_tts_prewarm()`） | 冷握手 3.5–4.3 秒只付一次；空闲 45 秒后重建（`TTS_IDLE_DROP_US`） |
| 静态内部 RAM 大头已知的两处 | 板文件注释 | I2S 环 8.6 KB（R6 再动）；LCD chunk 对 5.8 KB（R4 再动） |

### 1.4 调研报告 open question #2 已解：4608 到底是什么

调研报告（`~/workspace/research_notes/esp32-s3-memory-management-tts-lvgl-20261117-0233/report.md` §2.4）问：本地“16 KB→4608 B”是 `SPIRAM_MALLOC_ALWAYSINTERNAL` 还是 `MBEDTLS_SSL_IN_CONTENT_LEN`？**已核实：都不是**。

- 4608 是词卡预取门 `WC_MIN_INTERNAL_FOR_TLS 4608u`（`muse_wordcards_audio.c:78`），即“内部最大块 < 4608 B 时整组跳过、标 FAILED”（`:374-379`）。
- `SPIRAM_MALLOC_ALWAYSINTERNAL` 是 16384→**4096**（§1.3），不是 4608。
- `MBEDTLS_SSL_IN_CONTENT_LEN` 是 **16384**（`sdkconfig.defaults:132`），且在 PSRAM 里（`EXTERNAL_MEM_ALLOC=y`），没有被压过。

结论：btclock 的反例（Cloudflare 证书链 >4 KB 时 IN=4096 握手失败）**不适用于本板**——本板 IN 从未压到 4 KB。门槛 4608 本设计不动，只当验收观测点（§4 A3）。

## 2. 改动清单（逐项：文件、收益、风险、回退）

实施顺序建议：先 R1+R2+R4（小、可单项回退、收益代码可算）→ 实测 heartbeat 后决定 R3/R6/R7 → F1 单独一版（改生成器 + 解码路径）。每项一个提交，可 `git revert` 单项。

### R1. `muse_aplay` 任务栈进 PSRAM —— 代码可算 12,288 B

- **改什么**：`esp32/components/muse/muse_audio_play.c:296`，`xTaskCreate(play_task, "muse_aplay", TASK_STACK, …)` → `xTaskCreateWithCaps(…, MALLOC_CAP_SPIRAM)`；`TASK_STACK 12288`（`:41`）不变。
- **为什么能做（已核实）**：任务体只做 HTTP 取流 + minimp3 解码 + `muse_audio_write`（`:120-300`），解码缓冲已在 PSRAM（失败分支文案即“no PSRAM for the decode buffers”），体中无 NVS/Flash 写。与已落地的同类任务同形：`muse_tts` 12288 PSRAM、`muse_wc_fetch` 12288 PSRAM、`muse_wc_play` 24576 PSRAM——本任务是同类里唯一还留在内部栈的。
- **收益**：代码可算 **12,288 B**（播放进行时）；须实机量测：播放期间 heartbeat 的 `int` 最大块是否抬升约 12 KB。
- **风险**：低。PSRAM 栈 + Flash 写期间被调度会 fault（esp_mp3_player 同款教训），但本任务不写 Flash；同类任务已有此暴露面，无新增类别风险。
- **回退**：一行改回 `xTaskCreate`。

### R2. Setup 任务栈进 PSRAM（移植上游 7b302e3 的 helper 机制）—— 代码可算 20,480 B（峰值）

- **上游对照**：`7b302e3`（2026-11-12，StopWatch）：`app_hatch_vm_credentials()` 的 8 KB 任务起不来（最大块 ~7 KB / live 通话时 ~4 KB），DMA 池 8–10 KB 低于 Link 发 `link.register` 要等的 16 KB。修法：`helper_task_create()` + 板 overlay 开 `CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM` + `SPIRAM_MALLOC_ALWAYSINTERNAL` 4096→1024。Kconfig 帮助原文（上游 `esp32/main/Kconfig.projbuild`）：“sparing about 16 KB of internal RAM while setup runs … It needs code and read-only data in PSRAM: flash writes then leave the cache on, and provisioning writes NVS, which asserts on a PSRAM stack otherwise.”
- **改什么**（本板）：
  1. 新增 `esp32/main/helper_task.h`（照搬上游同名文件：`helper_task_create` / `helper_task_delete`，`CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM && CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA` 时栈走 PSRAM，否则走内部；WithCaps 建的任务必须用 `vTaskDeleteWithCaps` 删，普通 `vTaskDelete` 泄漏栈与 TCB）。
  2. `esp32/main/Kconfig.projbuild` 加 `HOMEHUB_SETUP_TASKS_IN_PSRAM` 选项（照搬上游文本，`depends on SPIRAM && SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA`，默认 `n`）。
  3. `esp32/devices/sdkconfig.muse-waveshare-s3-touch-lcd-185b` 开 `CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM=y`。
  4. `esp32/main/app.c` 四处改调用 + 四处改结尾 `vTaskDelete(NULL)` → `helper_task_delete(NULL)`：
     - `hatch_vm_task`（`muse_vm`，8192，`:2569`；结尾 `:2559`）——上游改的正是这个任务。
     - `vm_auth_refresh_task`（`vm_auth_rfsh`，8192，`:1210`；结尾 `:1171`）——本 fork 自有任务，与上游任务同形（短命、NVS token 刷新写）。
     - `scan_refresh_task`（`scan_rfsh`，4096，`:1413`；结尾 `:1395`）——上游 Kconfig 帮助明确列出“Wi-Fi scan refresh”。
- **前置条件已核实满足**：本板 overlay 已开 `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y`、`CONFIG_SPIRAM_RODATA=y`、`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`，即代码与 rodata 已在 PSRAM，Flash/NVS 写不关 cache——正是上游要求的前提。
- **收益**：代码可算 **20,480 B**（8192+8192+4096）；须实机量测：配对/VM lookup/扫描期间 heartbeat 的 `int` 最大块是否抬升约 20 KB；三任务栈水位（`stack_monitor_record`）≥ 512 B。
- **风险**：中低。PSRAM 栈 + Flash/NVS 写：前置条件已核实（FETCH/RODATA/XIP 均开），上游同款修法已在 StopWatch 上验证；本板三任务中 `vm_auth_refresh` 写 NVS token、`hatch_vm` 在 token 刷新时写 NVS、`scan_refresh` 只读扫描结果，均在“代码与 rodata 在 PSRAM、Flash 写不关 cache”的前提覆盖内。
- **回退**：overlay 关掉 `CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM`（helper 回落内部栈），或 revert 三处调用。

### R3. `SPIRAM_MALLOC_ALWAYSINTERNAL` 4096→1024 —— 量级须量测，本批不做

- **改什么**：`esp32/devices/sdkconfig.muse:35`，`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 4096→1024（≥1 KB 的 malloc 改走 PSRAM）。
- **收益**：须实机量测，量级未知；方向有 ESP-BOX/StopWatch 先例（上游 `7b302e3` 同笔，StopWatch 上给 Link 留出 16 KB DMA 保留区）。
- **风险**：小对象进 PSRAM 略慢；另注意 `sdkconfig.muse` 是全板共用底档，改动影响全部 full-UI 板——定稿前须决定放底档还是只放 185b overlay。
- **状态**：本批不做，实测 R1+R2+R4 的 heartbeat 后再定。

### R4. `LCD_CHUNK_BYTES` 4 行→2 行 —— 代码可算 2,880 B（已实施）

- **改什么**：`esp32/components/muse/boards/board_waveshare_s3_touch_lcd_185b.c:217`，`(LCD_H_RES * 4 * 2)` → `(LCD_H_RES * 2 * 2)`；上方注释同步。`DRAW_BUF_LINES`（`:211`，90 行 bands）不动。
- **收益**：代码可算 **2,880 B**（两 chunk 缓冲 5760→2880 B）；须实机量测：滑动流畅度、TE 时序。
- **风险**：低。每 band SPI 传输数相对 8 行时代 ×4（相对 4 行 ×2），每传输约 0.3 ms。
- **跟进修法（已含在本批）**：`ptest_lcd()` 黑清循环步长 8→2 行。1.5h 把 chunk 8→4 行时没改循环步长，`black` 缓冲（`LCD_CHUNK_BYTES`）只有 2880 B 却每次 `draw_bitmap` 读 5760 B，已越界；R4 改 2 行后越界 4320 B，故循环必须同步改（只诊断 panel-test 路径，非常规启动路径）。

### R5. 音频静态缓冲进 PSRAM —— 代码可算 4,480 B，合入前须验证拷贝语义

- **改什么**：`esp32/components/muse/muse_audio.c:91-92`（`s_in_frame` 2560 B + `s_out_stereo` 1280 B，`MUSE_AUDIO_CHUNK` 320）与 `esp32/components/muse/muse_voice.c:141`（`s_chunk` 640 B），加 `EXT_RAM_BSS_ATTR`；合计 **4,480 B**。
- **前置验证（合入前必须做）**：核对 esp_codec_dev 的 I2S 路径是拷贝语义——若 codec dev 直接 DMA 读该缓冲，PSRAM 缓冲不可用，R5 作废。chirp 的 PSRAM 缓冲经 `write_locked` 拷贝，不算证明。
- **状态**：本批不做，验证通过后再排。

### R6. I2S 环 6×120→6×60 —— 代码可算 4,320 B，本批不做

- **改什么**：板文件 `dma_frame_num` 120→60（`:1255`）。
- **收益**：代码可算 **4,320 B**（环 8.6 KB→4.3 KB）；须实机量测：DMA 欠载/爆音（每方向余量 45→22.5 ms）。
- **状态**：本批不做，实测后定。

### R7. Wi-Fi 静态 TX 8→4 —— 代码可算约 6,400 B，本批不做

- **改什么**：185b overlay `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` 8→4（`:63`）。
- **收益**：代码可算约 **6,400 B**（4×1.6 KB）；须实机量测：语音上行/OTA 吞吐。
- **状态**：本批不做，实测后定。

### F1. Art RLE 压缩进 flash —— 模型估算约 269 KB，单独一版

- **改什么**：Art RLE 压缩进 flash（先只做词卡）：改生成器 + 解码路径；运行时代价为 PSRAM 解码缓冲（每图 16 KB）。
- **收益**：离线模型估算（2 B/run，按实际字节算出，非编解码实测；已逐文件验算）：词卡省 **187,032 B**（raw 327,680 B，零字节 60.2%），四组（词卡/学习/主网格/运势）全量省 **268,982 B**（raw 493,570 B）；须实测编解码后节余 + 解码耗时 + 进页耗时。
- **状态**：单独一版，本批不做。

## 3. 不做的项（只评估没动手，原因写清）

1. **绘图缓冲移 PSRAM**：本板已是 bands in PSRAM；QSPI DMA 读 PSRAM 太慢、会回退内部 bounce（板文件注释 + `muse_lcd_bands.h`）；`d502649` 是双缓冲直连架构，不适用。
2. **`MBEDTLS_DYNAMIC_BUFFER=y`**：已有 `n` 且注释写明原因（其 heap 路径不走 `EXTERNAL_MEM_ALLOC`，会碎内部 SRAM）。
3. **`MBEDTLS_SSL_IN_CONTENT_LEN` 压 4K**：IN 16K 在 PSRAM，从未压过；btclock 的反例（Cloudflare 证书链 >4 KB 时 IN=4096 握手失败）不适用本板。
4. **`KEEP_PEER_CERTIFICATE=n`**：省的是 PSRAM 不是内部（`EXTERNAL_MEM_ALLOC=y` 下证书在 PSRAM）。
5. **TLS 会话恢复**：调研未验证，无 IDF 开关。
6. **`DRAW_BUF_LINES` 90→45**：省的是 PSRAM 绘图缓冲（129,600 B），不是瓶颈的内部 RAM；减半可能影响 band 吞吐与撕裂，须实测，排后批评估。
7. **`ws_control` 8192 栈进 PSRAM**：未核对其任务体有无 NVS/Flash 写与外部删除点，排后批评估。
8. **CJK 字库子集化**：字库 567,343 B 是 flash 大头之一，但子集化要动生成脚本与字表，另立一版评估。
9. **`90fc339` 直搬**：fork 无 `muse_voice_call.cpp`（无 live-call 路径）；其手法已用于 `s_turn`；类比项是 R5。
10. **`LV_MEM_SIZE` 调整**：LVGL 用 custom malloc 直走 PSRAM，不适用。
11. **`RESERVE_INTERNAL` 64K 不动**：64 KB 内部保留池是显式内部/DMA 分配的底线，动它先要有实测依据。
12. **4608 门槛不动**：`WC_MIN_INTERNAL_FOR_TLS` 只当验收观测点（§4 A2）。

## 4. 实机验收清单

方法：heartbeat 分池打点已存在（`app.c:3136-3154`），串口日志看 int/dma/psram 空闲/最大块/低水位；词卡预取门日志 `prefetch skipped`（`muse_wordcards_audio.c:377`）。

- **A1 内部最大块**：heartbeat `largest_int ≥ 8 KB`（硬判据，`muse_vm` 任务 8 KB 栈）；目标 DMA 池最大块 ≥ 16 KB（Link 发 `link.register` 要等的值）。
- **A2 `prefetch skipped` 归零**：重启后首次进词卡组，预取不再因内存门跳过；门槛 4608 不动，只当观测点。
- **A3 TTS 首音**：单词首点、听力首题出声时间，对照冷握手 3.5–4.3 秒基线；不再出现“重启后首次点按静默”。
- **A4 OOM 复现**：Muse 连接页不再报 out of memory。
- **A5 bin 大小**：F1 落地后 bin ≤ 4,042,752 B（按 148 KiB 取严，留出唤醒词空间）；本批 R1+R2+R4 不动 flash，bin 应基本不变。
- **A6 任务栈水位**：helper 三任务 + `muse_aplay` 栈水位 ≥ 512 B。
- **A7 回归**：语音回合、词卡、听力、掷筊摇动、OTA、轻睡唤醒均正常。
- **A8 显示回归**：滑动流畅度、TE 时序、无爆音（R6 实施后才适用）。

### 自 1.5.3e 结转的待验收项（状态不明，待用户确认，不虚构“已验收”）

- 掷筊翻一圈观感。
- 词卡重启后首次点按出声（与 A3 部分重合，单列以免再丢）。
- 整页卡住断档日志（抓一条断档日志确认卡死消失）。

## 5. 落地记录（本批）

- R1+R2+R4 已实施，提交 `a6a3d8a`（一笔代码提交，含 R4 的 ptest 循环跟进修法与一处 host 测试断言同步改名；未按“每项一个提交”，因分支约定三笔提交结构）。
- 本设计文档在 `6ef3640` 时曾截断（R2 句中断、缺 R3–R7/F1/§3/§4，末尾带截断标记），在本文档小修提交里补全；补写部分的数字已逐项验算（R5 三缓冲 2560+1280+640 B、F1 RLE 模型按实际字节验算）。
- host 测试：636 个，失败 27、错误 8、跳过 10，与基线逐行相同、零新增（日志 `~/workspace/memopt-after.log`）。
- 未编译（本机无工具链）；实机验收由用户做（`tools/muse/board.sh build lcd185b`，IDF 6.0.3）。
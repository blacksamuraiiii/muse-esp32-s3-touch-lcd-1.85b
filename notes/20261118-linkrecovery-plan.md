# 1.5.4 连接恢复移植设计（上游 481743f 对照）

- 日期：2026-11-18
- 基点：main `803ca78`（1.5.3 正式版），分支 `feat/linkrecovery-20261118`
- 上游提交：`facebookincubator/muse-gadget-sdk` `481743f`「esp32: recover from dead connections and stuck tunnels, and keep a connectivity history (#372)」，2026-11-11，25 文件，+1647/−72
- 性质：**只设计，不实现**。本文件是指导实现代理动手的手册：每项改动写清文件、函数、行为变化、风险、回退办法。
- 上游读物：上游克隆在 `/tmp/muse-upstream-linkrecovery`（只读，留给实现代理用）；完整 diff 已存 `/tmp/upstream-481743f.diff`（2654 行）。
- 目标口径（诚实版）：**host 测试只能证明机制，不能证明真实断网/隧道行为**。所有涉网结论一律写「须实机量测」，不要把推导写成实测。上游提交信息自己也只写了 "Build and static verified; hardware behaviour is the authors' report"。

## 0. 问题陈述（上游原话 + 我方现状）

网络抖动后，Link 可能把一个已死的会话或已死的隧道攥在手里好几分钟，且事后几乎不留证据：

- 半开 TCP：WAN 断了又回来、NAT 映射已丢，写操作照常成功，lwIP 的重传预算要 15 分钟以上才报发送失败；
- 隧道后端挂了但 WebSocket/Noise 传输还活着：隧道 keepalive 只能把隧道标 down、然后无限 reopen，服务端却因流 id 已被占用而拒绝每一次重试（"duplicate id"）；
- DMA headroom 长期关门：隧道外发队列永远发不出去，控制面却还活着，于是会话永远不拆，DMA 内存永远不释放；
- 事后无据可查：Wi-Fi/会话/隧道变迁没有任何留存，`device.health` 看不到历史。

我方现状：`esp32/main/noise_control.cpp`（2748 行）、`noise_tunnel.cpp`（370 行）与上游 `481743f` 之前基本一致（逐行核对过，见 §1），所以上述四个坑我方全有。好消息是移植面小：我方文件结构与上游同款，改动可按上游 diff 逐项落点，只有 `send_device_health`、会话循环的 early-return 风格等几处须适配（见 §2）。

不属于本次范围：**链路换段**（局域网 → 腾讯云 → CF relay.example.com → 美国 VPS → Muse 服务器）一概不动，只做固件侧自愈与诊断；内存优化分支 `feat/memopt-20261117` 未合 main，不以它为基点、不引用它的改动；上游另一笔 `88b04a3`（删 `device.ota`）只记在 §6，不动手。

## 1. 上游机制逐项对照表

阈值总览（上游原值，本次**全部照搬、不调参**；调参须有实机数据再定）：

| 阈值 | 值 | 含义 |
|---|---|---|
| `TUN_KEEPALIVE_INTERVAL_US` | 20 s | 隧道空闲后发 keepalive ping（已存在，不动） |
| `TUN_KEEPALIVE_TIMEOUT_US` | 65 s | 无入站隧道帧（pong 或数据）即判隧道死（值不动，只从 `noise_tunnel.cpp` 移到 `noise_tunnel_internal.h`，供会话循环共用） |
| `TUN_PING_PROBE_US` | 5 s | 隧道 ping 未答复满 5 s，会话补发一个 WebSocket ping 探活 |
| `TUN_PING_ANSWER_US` | 15 s | 只有未答复满 15 s 的 ping 才能证明连接已死（给 5 s 时发的 WS ping 留 10 s 等 pong） |
| `RX_IDLE_TIMEOUT_MS` | 150 s | 全连接无入站即重连（两声未答复的 keepalive 之后） |
| `KEEPALIVE_INTERVAL_MS` | 60 s | WS ping 间隔（值不动；语义放宽：见 M4） |
| `TUN_TX_STALL_TIMEOUT_US` | 30 s | 隧道外发/DMA 门控重开无进展即拆会话 |
| `TUN_TX_BLOCKED_RECORD_US` | 5 s | 自行恢复的短暂 TX 阻塞满 5 s 记一条历史 |
| `TUN_REOPEN_INTERVAL_US` | 5 s | 隧道重开限速（已存在，不动） |
| `LINK_EVENTS_CAPACITY` / `HEALTH_MAX` / `DETAIL_MAX` | 128 / 40 / 112 | 环容量 / `device.health` 取最新条数 / 单条 detail 上限（含 NUL） |
| `PROBE_TCP_TIMEOUT_MS` / `PROBE_STACK` | 3000 ms / 4096 B | TCP 探针单次超时 / 探针任务栈（优先级 2） |
| `RESET_REASON_MAX` / `TUNNEL_FAILURE_MAX` | 40 / 72 | 服务端 Reset 原因保留上限 / 隧道失败描述保留上限（含 NUL） |
| `BUG_REPORT_MAX_BYTES` | 32 KiB | 不变（`bug_report.h:24` 已是该值） |

### M1. 连接历史环 `link_events`（新模块）

- **上游做法**：新文件 `esp32/main/link_events.{c,h}`。PSRAM 环 128 条，每条 `{at_us, dma_free, dma_largest, kind, detail[112]}`；`link_events_init()` 用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，无 PSRAM 则返回 false、记录变 no-op；`link_events_record(kind, fmt, ...)` 每次取当前 DMA 空闲/最大块、mutex 保护（任何任务可调，**禁 ISR**）、永不打 log；`link_events_add_json(parent, name, max)` 把最新 max 条以 `"<N>s ago <kind>: <detail> (dma_free=<B> dma_largest=<B>)"` 写成字符串数组（最旧在前），先整环拷出再格式化、不持锁格式化。8 种 kind：`wifi_down/wifi_up/session_up/session_down/tunnel_up/tunnel_down/tunnel_tx_blocked/stall_probe`。环约 128×~132 B ≈ 17 KB PSRAM；`add_json` 另有一次整环拷贝（约 17 KB PSRAM 瞬时占用）。
- **我方现状**：无此模块。`diagnostic_log.c` 只有文本日志环（PSRAM），一次断网日志突发就能把致因顶掉；`device.health`（`noise_control.cpp:1692` 起静态 `send_device_health`）无任何历史字段。
- **结论：移植**。近乎原文搬运；见 §2.1。

### M2. 静默断开后的 TCP 探针 `link_probe`（新模块）

- **上游做法**：新文件 `esp32/main/link_probe.{c,h}`。`link_probe_on_stall()` 用 atomic 标志防重入，起一个独立低优先级任务（栈 4096 B，优先级 2），跑完自删。任务内：Wi-Fi 未连 → 记 `"wifi down"`；否则按 IP 直连 `1.1.1.1:443`（3 s 超时，非阻塞 connect + select），失败再试 `8.8.8.8:443`；任一通（`ok` 或 `refused` 都算"到达"）→ 记 `"internet ok"`，否则记 `"internet unreachable (1.1.1.1 <r1>, 8.8.8.8 <r2>)"`。不发任何数据、不做 DNS。结果以 `LINK_EVENT_STALL_PROBE` 记进历史环，并打一条 `ESP_LOGW`。任务结束调一次 `stack_monitor_record(NULL)`。
- **我方现状**：无此模块。所需依赖都已存在：`wifi_mgr_is_connected()`（`wifi_mgr.h:79`）、`stack_monitor_record()`（`stack_monitor.h:33`）。
- **结论：移植**。见 §2.2、§5（探针在用户链路下的意义与局限）。

### M3. 本地标死隧道流后，重开前先发 Reset

- **上游做法**：`noise_tunnel_emit_t` 加 `reset_stream` 回调（`noise_tunnel_internal.h`）。`noise_control.cpp` 新增 `tunnel_reset_stream()`：`session->StartOutboundReset(ServiceType::Daemon, TUNNEL_STREAM_ID, {ResetCode::Cancelled, "tunnel reopen"}, svc/env scratch)` + `flush_outbound()`。`noise_tunnel.cpp` 加 `s_needs_reset`（会话任务独占）：keepalive 超时、ping 发送失败时置位；`noise_tunnel_on_session_up()` 清零；`maybe_reopen()` 在重开前若置位则先调 `reset_stream`，失败 → 返回 false（拆会话）；成功后清零。服务端主动 Reset / 拒开（非 200）时**不**置位——服务端已经忘了这个流 id，下一次是裸 open。`try_open_stream()` 成功后才把 `s_needs_reset` 语义清掉（靠 `on_session_up` 与成功 Reset 两处）。
- **我方现状**：emit 结构体只有 4 个字段（`noise_control.cpp:2221-2222`：`{&tun_ctx, tunnel_open_stream, tunnel_send_body, tunnel_send_body_inplace}`），`maybe_reopen` 是 `void`（`noise_tunnel.cpp:262`），本地标死后直接重试 open，服务端会以 "duplicate id" 拒绝每一次重试，直到会话重启。`StartOutboundReset` 本身可用：`noise_core` 的 `ClientSession.h:105` 有该方法，我方 `noise_control.cpp:1198`（cancel 某请求）与 `components/muse/muse_chat_session.cpp:692` 已有调用先例；`tunnel_emit_ctx`（`noise_control.cpp:965`）已有 `tls/session/svc_scratch/env_scratch/ws_buf`，与上游 `tunnel_reset_stream` 要用的字段一致。
- **结论：移植**。见 §2.3。

### M4. 150 秒无声重连 + WS ping 也按 RX 空闲发

- **上游做法**：`noise_control.cpp` 新增 `RX_IDLE_TIMEOUT_MS 150000`。会话循环：`last_rx_us` 只由入站刷新（应用帧、keepalive pong、`n == -3` 的 WS 控制帧）；`last_ws_ping_us` 新增。原来 `if (!did)` 才按 TX 空闲发 ping，改为每回合算三条：`rx_idle_us >= 150 s` → `end_reason = "no response from VM"`，`went_silent = true`，拆会话重连；`tx_idle`（无事可做且 TX 空闲 ≥60 s）或 `rx_idle`（RX 空闲 ≥60 s 且距上次 WS ping ≥60 s）→ 发 WS ping，`last_activity_us = last_ws_ping_us = now`。即：**只发不收时也 ping，且每 60 s 最多一次**；入站一恢复就停。
- **我方现状**：只有 TX 空闲 ping（`noise_control.cpp:2465-2475`：`if (!did)` 且 `idle_us >= 60 s` 才 `ws_send_ping`）。`last_rx_us` 已存在（`:2209-2211`，语义与上游一致：入站刷新；`n == -3` 的 pong 也刷新，见 RX 循环）。无 RX 空闲超时：半开连接上只要我方还在发（心跳、隧道 ping 密封成功都算 `did`/TX），`last_activity_us` 一直刷新，ping 永远不发、会话永远不拆——这是本次要修的核心坑之一。
- **结论：移植**。见 §2.4。注意：隧道开着时 M6 的 65 s 判定先触发，150 s 是隧道未开/已关时的兜底。

### M5. DMA 卡住 30 秒拆会话（TX 停滞 + 重开门控）

- **上游做法**：`noise_tunnel.cpp` 新增 `TUN_TX_STALL_TIMEOUT_US 30 s`、`TUN_TX_BLOCKED_RECORD_US 5 s`。`pump_tx()`：队列非空且 DMA 门连续关 30 s 无进展 → 记 `tunnel_tx_blocked`（含最小 `dma_free` 与阻塞秒数，detail 形如 `"outbound blocked 30s on DMA headroom (min dma_free=0); tearing down session"`），返回 `-1`，会话循环随之拆会话重连。自行恢复的短暂阻塞也留痕：`end_tx_stall()` 在阻塞结束时，若已持续 ≥5 s，记一条 `tunnel_tx_blocked`（`"outbound blocked %us on DMA headroom (min dma_free=%u); resumed"` / `"queue drained"` / `"queue dropped by reopen"` / `"session ended"`）。同一路门控也管重开：`maybe_reopen()` 里 Reset + Request 的密封走 AES DMA 路径，DMA 门关着时不消耗重开限速（`s_last_open_attempt_us` 不更新），只累计 `s_outage_dma_wait_us`；连续被门控满 30 s → 返回 `false`，拆会话（控制面流量会一直让会话活着，等下去隧道可能永远打不开，而拆会话能释放 TLS 缓冲与 DMA 区的 socket pbuf）。
- **我方现状**：`pump_tx()`（`noise_tunnel.cpp:290`）只在发送出错时返回 `-1`，无停滞计时、无 `tunnel_tx_blocked` 记录；`maybe_reopen()` 是 `void`（`:262`），重开不查 DMA 门、不返回拆会话信号。DMA 门本身已存在（`noise_tx_has_dma_headroom` / `noise_tx_has_contiguous_dma_headroom`），只缺"卡住就拆"的判定。
- **结论：移植**。见 §2.5。**实现注意**：拆会话前须核对隧道 TX 生产者——`pump_tx()` 返回 `-1` 时会话循环是 `error = true; break;`（`noise_control.cpp:2434-2438` 现状），队列里未发出的 batch 会随会话 teardown 丢弃，这是上游有意为之（释放 DMA 内存优先于保住队列），不是 bug；回退办法见 §2.5。另注意 `s_tx_stall_min_dma` 取阻塞期间的最小值，恢复后清零（`end_tx_stall`），不要把一次阻塞的最小值带到下一次。
### M6. 65 秒隧道超时判重连 vs 重开（未答复 ping 分流）

- **上游做法**：`noise_tunnel.cpp` 新增 `s_unanswered_ping_us`（最早一个未答复 keepalive ping 的发送时刻，无未答复 ping 时为 0）、`s_timeout_ping_us`（`noise_tunnel_take_unanswered_timeout()` 一次性取走）、`s_last_rx_s`（`std::atomic<uint32_t>`，秒级，供跨任务读）。`noise_tunnel_tick()` 里 keepalive 超时（65 s 无入站隧道帧）标 down 时：只有当 `s_unanswered_ping_us` 非 0 **且**该 ping 已等满 `TUN_PING_ANSWER_US`（15 s）才把它的发送时刻存进 `s_timeout_ping_us`；ping 全被 DMA 门挡住没发出去、或刚发出去还没来得及等，都存 0——这两种情况都不能证明连接已死。会话循环在 `tick()` 之后取一次 `noise_tunnel_take_unanswered_timeout()`：取到非 0 **且** `last_rx_us < unanswered_ping_us`（自那个 ping 发出后全连接再无任何入站）→ `end_reason = "no response from VM (tunnel keepalive)"`，`went_silent = true`，拆会话重连；取到 0 或其间有入站（例如 WS ping 的 pong 到了）→ 只重开隧道，不断会话。另有一条探活支线：隧道 ping 未答复满 `TUN_PING_PROBE_US`（5 s）且其间无入站、且上次 WS ping 也是在该隧道 ping 之前发的，会话循环就补发一个 WS ping（`tun_probe`），给活连接一次在隧道 65 s 超时之前自证的机会；这条 WS ping 也受 M4 的"每 60 s 最多一次"以外的独立节流（`last_ws_ping_us < tun_ping_us` 保证对同一个隧道 ping 只补发一次）。`TUN_KEEPALIVE_TIMEOUT_US` 从 `noise_tunnel.cpp` 的局部 `#define` 移到 `noise_tunnel_internal.h`，供会话循环与隧道共用；`TUN_PING_PROBE_US`（5 s）与 `TUN_PING_ANSWER_US`（15 s）也定义在该头文件。
- **我方现状**：`noise_tunnel_tick()` 只有"65 s 无入站 → 标 down、等 `maybe_reopen` 重开"一条路，不区分"隧道后端挂了"与"整条连接死了"；会话循环没有 `take_unanswered_timeout` 的取用点。`TUN_KEEPALIVE_TIMEOUT_US` 目前是 `noise_tunnel.cpp` 内的局部宏，会话循环读不到。
- **结论：移植**。见 §2.6。**关键语义**：65 s 是隧道判定，重连/重开的分流看的是"自最老未答复 ping 发出后有没有入站"，不是看 65 s 本身；`s_last_rx_s` 必须用 32 位 atomic——ESP32-S3 是 32 位核，64 位 `s_last_rx_us` 跨任务读会撕裂，上游注释点名的是 C5，同理适用于 S3，不要图省事直接读 64 位。

### M7. 服务端 Reset 原因记历史（`on_server_drop`）

- **上游做法**：新增 `noise_tunnel_on_server_drop(const char *why)`（`noise_tunnel.cpp` / `noise_tunnel_internal.h`），与"会话结束"的 `noise_tunnel_on_session_down()` 分开：前者只标隧道 down 并记一条 `tunnel_down`（`why` 非空时写进 `s_outage_last_failure`，供恢复时汇总进 `tunnel_up`）；后者 `why` 为空，复用会话 down 事件的原因。会话循环两处改用它：服务端 Reset 隧道流时，`copy_reset_reason()` 把线上的 reason 先洗成可打印 ASCII（其余字符变 `?`）、截到 `RESET_REASON_MAX`（40 字节，含 NUL——上游注：服务端最长的 reason `"chunk for stream with completed body"` 是 36 字节），再拼成 `why`（`"server reset stream, code=%d: %s"`，`TUNNEL_FAILURE_MAX` 72 字节含 NUL 封顶）；服务端拒开（非 200）时 `why` 为 `"server rejected open: %d"`。**注意**：reason 只能说明是 Reset，不能说明是哪一种服务端 `ProtocolError`，不要在诊断里反推。M3 的不置位规则与此配套：服务端主动 Reset / 拒开后 `s_needs_reset` 不置位（服务端已经忘了这个流 id，下一次是裸 open）；只有本地标死（keepalive 超时、ping 发送失败）才置位。
- **我方现状**：两处都调 `noise_tunnel_on_session_down()`（`noise_control.cpp` 的 Reset 分支与拒开分支），只打一条 `ESP_LOGW("server reset tunnel stream")` / `"server rejected /link-tunnel: %d"`，原因不进任何留存；也没有 `copy_reset_reason` / `RESET_REASON_MAX` / `TUNNEL_FAILURE_MAX`。
- **结论：移植**。见 §2.7。

### M8. `device.health` 加 tunnel 计数器 + `connectivity_events`

- **上游做法**：`send_device_health` 从静态 `void` 改为导出的 `extern "C" cJSON *noise_ctrl_device_health_json(bool for_bug_report)`（声明进 `noise_control.h`，调用方拥有返回对象），原静态 `send_device_health(session_generation, request_id)` 变薄壳：调它取对象、序列化、进 `queue_result`。新增字段（只在 `CONFIG_HOMEHUB_TUNNEL` 下）：`tunnel_connected`（`noise_tunnel_is_connected()`）、`tunnel` 对象——`rx_idle_seconds`（`noise_tunnel_rx_idle_s()`，从未开过流时为 `UINT32_MAX`，此时**不写该字段**）、`rx_packets` / `rx_bytes` / `tx_packets` / `tx_bytes` / `tx_dropped`（`tunnel_netif_get_stats()`）。`connectivity_events` 由 `link_events_add_json(health, "connectivity_events", for_bug_report ? LINK_EVENTS_CAPACITY : LINK_EVENTS_HEALTH_MAX)` 写入，即 `device.health` 只给最新 40 条、`bug.report` 给全部 128 条。SSID 只在 `!for_bug_report` 时写（`bug.report` 不带 SSID）。`link.register` 里 `device.health` 的命令描述同步改写，点明含 `connectivity_events`。**语义提醒**：`tunnel_connected` 只表示流开着，不表示在走流量——要结合 `rx_idle_seconds` 与计数器读。
- **我方现状**：`send_device_health` 是静态 `void`（`noise_control.cpp:1692`），只有 `overall` / `levels` / `critical` / `warnings` / `metrics`（uptime、堆、电池）/ `network_ssid` / `version`，无 tunnel、无历史。`noise_control.cpp` 目前没有 `#include "noise_tunnel.h"` 与 `#include "tunnel_netif.h"`（上游 diff 在该文件头部补了这两处 include，另补 `link_events.h` / `link_probe.h`）；`device.health` 的注册描述在 `:1405` 起。
- **结论：移植**。见 §2.8。**适配点**：我方 `send_device_health` 是内联静态版，改成导出函数时须确认没有别的同名/同签名静态函数冲突，且 `queue_result` 的调用点不变；`tunnel_netif.h` 的 include 路径以我方树为准。

### M9. `bug.report` 内嵌 `device.health`，超 32 KiB 丢最旧事件

- **上游做法**：`bug_report.c` 的快照删掉 `tunnel_connected` 与 `tunnel_stats_t` 字段（不再直接采隧道计数器），`build_report_json` 里删掉顶层 `"tunnel"` 对象，改为嵌入 `noise_ctrl_device_health_json(true)` 的返回值作 `"device_health"`（失败则整段缺省、报告其余部分照常出，不让一次 health 失败毁掉整份报告）。超限处理：`BUG_REPORT_MAX_BYTES`（32 KiB，`bug_report.h:24`，值不动）仍是硬上限；序列化后若超限，循环调用新增的 `drop_oldest_events()`——每次删 `connectivity_events` 数组里最旧一条（按"字符串长 + 引号 + 逗号"估算省下的字节数），直到 fits 或数组删空；日志尾（`tail_base64`）全程保留。harness 覆盖三点：health 失败时报告仍有 `"logs"` 且无 `"device_health"`；满载 128 条最长事件时报告 ≤32 KiB、留最新（`e127` 在、`e000` 不在）、日志尾完整；`device_health` 里无 `network_ssid`（不泄 SSID）。
- **我方现状**：`bug_report.c:149-150` 快照仍采 `noise_tunnel_is_connected()` + `tunnel_netif_get_stats()`，`:257` 起顶层 `"tunnel"` 对象；`BUG_REPORT_MAX_BYTES` 已是 32 KiB（`bug_report.h:24`），但超限时目前是整份报告丢弃（`report_len > BUG_REPORT_MAX_BYTES` 即返回 NULL），没有"丢最旧事件保报告"的降级路径。
- **结论：移植**。见 §2.9。**风险**：`drop_oldest_events` 的字节估算（`strlen + 3`）是近似值，循环以实测 `strlen(report)` 为准、不会少删；但它假设 `device_health` / `connectivity_events` 节点存在，`cJSON_GetObjectItem` 取不到时返回 false 即停，不要在 health 缺省时继续循环。

### M10. Wi-Fi up/down 变迁记历史

- **上游做法**：`wifi_mgr.c` 的 `event_handler` 只记 IP 层变迁、不记每一次重试：新增 `s_has_ip` / `s_ip_lost_us` 两个静态量。`WIFI_EVENT_STA_DISCONNECTED` 时，若此前有 IP（`s_has_ip`），置 false、记 `s_ip_lost_us = now`、记 `wifi_down`（`"esp-idf reason=%u rssi=%d"`）；此前无 IP（启动后从未拿到 IP、或已在断开态）则不记，避免重连重试刷屏。`IP_EVENT_STA_GOT_IP` 时，若此前无 IP，置 true；若 `s_ip_lost_us` 非 0，记 `wifi_up`（`"got IP after %llus without Wi-Fi"`），否则记 `"got IP"`。新增 `#include "esp_timer.h"` 与 `#include "link_events.h"`。
- **我方现状**：`wifi_mgr.c` 的 `event_handler`（`WIFI_EVENT_STA_DISCONNECTED` 在 `:327`，`IP_EVENT_STA_GOT_IP` 在 `:360`）只有日志与事件组位操作，无任何历史记录；`link_events.h` 未 include。
- **结论：移植**。见 §2.10。**注意**：`event_handler` 跑在 ESP-IDF 事件任务上，不是 ISR，调 `link_events_record` 合法（该函数持 mutex，禁 ISR 不禁任务）；但不要在这里加 `ESP_LOG` 以外的耗时操作，重连退避逻辑（`s_retry` / `s_reconnect_backoff_ms`）一字不动。

### M11. 会话 up/down + 失败尝试汇总 + `went_silent` 触发探针

- **上游做法**：`run_session()` 顶部新增三个会话任务独占量：`end_reason`（初值 `"stopped"`）、`went_silent`（初值 false）、`session_up_us`；另有文件级 `s_failed_attempts` / `s_last_attempt_failure` / `s_session_down_us`（`noise_ctrl_task` 独占）。行为分四块：① **失败汇总**：会话建立前失败（TLS init/connect、WS upgrade、auth 拒绝）直接 `return` 前调 `note_attempt_failed(reason)`（计数 +1、记最后一条原因）；会话建立后失败（见下）走 `goto cleanup`，`cleanup:` 处若 `session_up_us` 为 0（即从未 up 过）再补记一次 `note_attempt_failed(end_reason)`——两种路径互斥，不会 double-count。② **`end_reason` 赋值点**（逐行核数：`run_session` 内 19 处赋值、18 种原因字符串，`control stream open failed` 占两处——`StartOutboundApplicationRequest` 失败与 `flush_outbound` 失败；声明初值 `"stopped"` 另计，不在 19 处内）：buffer alloc、Noise 握手、control 流打开、link.register 构建、socket setup、identity 请求、入站帧拒绝、服务端 Reset control 流、服务端拒开 `/link-control`、对端关闭、socket 读错、control 发送失败、隧道重开失败、隧道 TX 失败/停滞、Noise 会话破裂、WS ping 失败、两条无声（M4 的 `"no response from VM"` 与 M6 的 `"no response from VM (tunnel keepalive)"`）。③ **会话 up/down 事件**：`s_connected = true` 后记 `session_up`——此前有失败尝试时 detail 为 `"established after %us: %u failed attempts, last: %s"`（耗时自 `s_session_down_us` 起算），否则 `"established"`，随后 `s_failed_attempts` 清零；teardown 处（`s_connected = false` 之后）记 `session_down`（`"%s after %us up, rx idle %us"`），并置 `s_session_down_us = now`。④ **静默触发探针**：只有 `went_silent` 为真（两条无声路径之一）才在 teardown 后调 `link_probe_on_stall()`；其余原因（auth 失败、对端关闭、发送失败等）不触发——探针只回答"静默是谁的错"，不回答所有失败。
- **我方现状**：`run_session()`（`noise_control.cpp:2036`）无 `end_reason` / `went_silent` / `session_up_us`，失败路径是裸 `return` / `goto cleanup` / `error = true; break;`，teardown 处（`:2486` 起）只有一条 `ESP_LOGI("session ending (rx idle %llus at teardown)")`，无历史、无汇总、无探针触发。
- **结论：移植**。见 §2.11。**声明位置是硬性要求**：`end_reason` / `went_silent` / `session_up_us` 必须声明在 `run_session()` 顶部、所有 `goto cleanup` 的目标之前——我方 `goto cleanup` 有多处（`:2129`、`:2138`、`:2158`、`:2186`、`:2197` 等），且 cleanup 标签后的代码会读 `end_reason`，若把声明放在某条 `goto` 之后（例如只在会话循环附近声明），编译器会报"跳过初始化"（C++ jump over initialization）。上游把三者放在 `run_session` 开头、`ws_buf` 等指针声明之旁，照搬该位置，不要挪。

### M12. 测试移植（host harness + 契约测试）

- **上游做法**：四组改动，全在 `esp32/tests/`：① 新 `link_events_harness.c` + `test_link_events.py`：host 编译 `link_events.c` + fakes（`LINK_FAKE_CUSTOM_HEAP_CAPS` / `LINK_FAKE_CUSTOM_TIMER`），跑环的写入/覆盖/取最新 40 条与格式化（含 `"4294967295s ago …"` 的大数格式）。② `link_noise_tunnel_harness.cpp` 加场景：DMA 门控重开（门关时 `maybe_reopen` 返回 true 且不消耗重试间隔、不发流操作；门开后先 Reset 再 open，`stream_ops == "ORO"`；服务端已丢流时裸 open `"OO"`；ping 发送失败后重开也要 Reset）、TX 停滞（阻塞 5 s 记一条 `tunnel_tx_blocked`、恢复后 `"resumed"`；满 30 s `pump_tx` 返回 -1；再阻塞 30 s 仍返回 -1 前先记第二条）、以及 `s_last_rx_s` 的 atomic 读。③ `link_noise_transport_harness.cpp` 加 `test_rx_idle` 与 `test_dead_connection` 两个场景（见 §2.12 的用例表），`test_link_noise_tunnel.py` 注册这两个用例并在切片常量里补 `RX_IDLE_TIMEOUT_MS`；`test_link_transport_contract.py` 把 `maybe_reopen` 的契约断言从 `void` 改为 `bool`（`"if (tunnel_opened && !noise_tunnel_maybe_reopen(&tun_emit))"` 与 `extern "C" bool noise_tunnel_maybe_reopen(`）；`test_link_bug_report.py` 补 M9 的三点断言（health 失败保报告、满载保最新事件与日志尾、health 里无 SSID），并在 harness 里用 `noise_ctrl_device_health_json` 的 fake 镜像真实形状（含 128 条最长事件的 `e000`–`e127` 编号）。④ `link_bug_report_harness.c` 删掉直采隧道计数器的 fake（`noise_tunnel_is_connected` / `tunnel_netif_get_stats`），改用 health fake。
- **我方现状**：harness 体系与上游同款：`test_link_noise_tunnel.py`（切片锚点在 `:39-90`，见 §2.12）、`link_noise_tunnel_harness.cpp`、`link_noise_transport_harness.cpp`、`test_link_bug_report.py`、`test_link_transport_contract.py` 都在；我方另有 `test_link_raw_tunnel.py`（上游没有，是我方分支自有的裸隧道测试，移植时不要动它、不要把它当成上游用例的落点）。
- **结论：移植**。见 §2.12。**host 测试的诚实边界**：这些 harness 只能证明"给定 fake 时间/ DMA 门状态，代码走了哪条分支"，不能证明真实断网、真实 NAT 超时、真实 DMA 压力下的行为；上游提交信息自己也只写 "Build and static verified; hardware behaviour is the authors' report"。所有涉网结论一律写「须实机量测」，不要把 harness 通过写成实测通过。

## 2. 逐项落点（实现手册）

每项的格式：文件 → 函数 → 行为变化 → 风险 → 回退。**通用回退**：本移植不改任何已存行为的默认阈值（全部照搬上游原值），任一单项出问题时，回退办法都是"把该项的改动整段 revert、保留其余项"，不要逐行打补丁；`link_events` / `link_probe` 两个新模块可整文件删（连同 CMakeLists 两行与 `main.c` 的 init 调用）而不影响其余项——但 M8/M9 依赖 M1 的 `link_events_add_json`，删 M1 时须同步把 M8 的 `connectivity_events` 与 M9 的 health 内嵌一并 revert。

### 2.1 M1：新模块 `link_events.{c,h}`

- **文件**：新建 `esp32/main/link_events.c`、`esp32/main/link_events.h`（近乎原文搬运，license 头保留）；`esp32/main/CMakeLists.txt` 的 `GADGET_SRCS` 加 `"link_events.c"`（上游放在 `diagnostic_log.c` 之后）；`esp32/main/main.c` 的 `app_main()` 里在 `diagnostic_log_init()` 成功之后加 `if (!link_events_init()) ESP_LOGW("link.main", "connectivity history disabled: PSRAM unavailable");`。
- **函数/行为**：环 128 条，每条 `{at_us, dma_free, dma_largest, kind, detail[112]}`；`link_events_init()` 用 `heap_caps_malloc(128×~132 B ≈ 17 KB, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，无 PSRAM 返回 false、记录变 no-op；`link_events_record(kind, fmt, ...)` 每次取当前 DMA 空闲/最大块、mutex 保护、永不打 log；`link_events_add_json(parent, name, max)` 先整环拷出再格式化（另一次约 17 KB PSRAM 瞬时占用），格式 `"<N>s ago <kind>: <detail> (dma_free=<B> dma_largest=<B>)"`，最旧在前。8 种 kind 见 M1。
- **风险**：① `add_json` 的整环拷贝在 `device.health` 的会话任务上跑，若此时 PSRAM 紧张可能分配失败——上游对此返回 false、health 里只剩空数组，不要改成阻塞等待；② `record` 持 mutex 用 `portMAX_DELAY`，调用方若在持另一把锁的路径上调它会死锁——本移植的所有调用点（会话任务、Wi-Fi 事件任务、探针任务）都不持锁，新增调用点时先核对。
- **回退**：删两文件 + CMakeLists 两行 + `main.c` 三行；其余文件里的 `link_events_record` 调用须同步删（否则链接失败），所以 M1 只能与 M7/M10/M11 同进同退，不能单独留调用点。

### 2.2 M2：新模块 `link_probe.{c,h}`

- **文件**：新建 `esp32/main/link_probe.c`、`esp32/main/link_probe.h`（原文搬运）；`esp32/main/CMakeLists.txt` 的 `GADGET_SRCS` 加 `"link_probe.c"`；不改 `main.c`（探针只由 M11 的 `went_silent` 触发，无需启动时 init）。
- **函数/行为**：`link_probe_on_stall()` 用 atomic 标志防重入，起独立任务（栈 4096 B、优先级 2，跑完自删）；任务内 Wi-Fi 未连 → 记 `"wifi down"`，否则按 IP 直连 `1.1.1.1:443`（3 s 超时，非阻塞 connect + select），失败再试 `8.8.8.8:443`；`ok` 或 `refused` 都算到达 → 记 `"internet ok"`，否则记 `"internet unreachable (1.1.1.1 <r1>, 8.8.8.8 <r2>)"`。不发数据、不做 DNS。结果以 `LINK_EVENT_STALL_PROBE` 记环，并打一条 `ESP_LOGW`；结束调一次 `stack_monitor_record(NULL)`。
- **风险**：① 任务创建失败（内存不足）时只打一条 `ESP_LOGW("stall probe task unavailable")` 并把 atomic 标志清回 false，不要重试、不要阻塞会话任务；② 探针任务与重连并行跑，重连成功后探针结果仍会写入历史——这是有意的（探针回答的是"静默发生时"那一刻的互联网可达性，不是现在的），读历史时按 `"<N>s ago"` 对时间，不要当成当前状态。
- **回退**：删两文件 + CMakeLists 一行，并把 `noise_control.cpp` 里 M11 的 `link_probe_on_stall()` 调用删掉（留调用不留模块会链接失败）。

### 2.3 M3：本地标死隧道流后重开前先发 Reset

- **文件**：`esp32/main/noise_tunnel_internal.h`（`noise_tunnel_emit_t` 加 `reset_stream` 回调字段）、`esp32/main/noise_control.cpp`（新增 `tunnel_reset_stream()`，`tun_emit` 初始化加第五个字段）、`esp32/main/noise_tunnel.cpp`（`s_needs_reset` 与 `maybe_reopen` 改 `bool`）。
- **函数/行为**：`tunnel_reset_stream(void *vctx)`：`session->StartOutboundReset(ServiceType::Daemon, TUNNEL_STREAM_ID, {ResetCode::Cancelled, "tunnel reopen"}, svc/env scratch)` + `flush_outbound()`；任一步失败返回 false。`noise_tunnel.cpp` 加会话任务独占的 `s_needs_reset`：keepalive 超时、ping 发送失败时置位；`noise_tunnel_on_session_up()` 清零；`maybe_reopen()` 在重开前若置位则先调 `reset_stream`——**失败 → 返回 false（拆会话），不重试、不裸 open**（Reset 密封失败即拆会话不重试：没 Reset 的 reopen 注定被服务端以 "duplicate id" 拒绝，重试只是空转）；成功后清零再 `try_open_stream()`。服务端主动 Reset / 拒开（非 200）时不置位。`try_open_stream()` 成功后语义上清掉（靠 `on_session_up` 与成功 Reset 两处）。
- **风险**：① `tunnel_reset_stream` 用的 `tunnel_emit_ctx` 字段（`tls/session/svc_scratch/env_scratch/ws_buf`，`noise_control.cpp:965`）与上游一致，照搬即可，但须确认我方 `StartOutboundReset` 的签名与上游同款（我方已有调用先例：`noise_control.cpp:1198`、 `components/muse/muse_chat_session.cpp:692`，先对签名再写）；② `maybe_reopen` 签名 `void` → `bool` 是全树改动，调用点只有会话循环一处（`noise_control.cpp:2430`），harness 与契约测试同步改（见 §2.12），不要漏掉 `noise_tunnel_internal.h:1439` 的声明。
- **回退**：把 `maybe_reopen` 改回 `void`、删 `reset_stream` 字段与 `tunnel_reset_stream()`、`s_needs_reset` 相关行；回退后本地标死隧道仍会陷入 "duplicate id" 死循环（即本次要修的坑之一），回退只是为了隔离故障，不是长期状态。

### 2.4 M4：150 秒无声重连 + WS ping 按 RX 空闲发

- **文件**：只改 `esp32/main/noise_control.cpp`（会话循环 + 新增 `RX_IDLE_TIMEOUT_MS 150000`）。
- **函数/行为**：`run_session()` 的会话循环：新增 `last_ws_ping_us`（初值 0）；每回合算三条——`rx_idle_us >= 150 s` → `end_reason = "no response from VM"`，`went_silent = true`，拆会话；`tx_idle`（本回合无事可做且距 `last_activity_us` ≥60 s）或 `rx_idle`（RX 空闲 ≥60 s 且距上次 WS ping ≥60 s）→ 发 WS ping，`last_activity_us = last_ws_ping_us = now`。即只发不收时也 ping、每 60 s 最多一次，入站一恢复就停。`last_rx_us` 的刷新点不动（应用帧、keepalive pong、`n == -3` 的 WS 控制帧；我方 `:2209-2211`、`:2336-2337` 语义与上游一致）。**150 秒判定依赖 WS pong 路径**：pong 必须能刷新 `last_rx_us`——我方 `n == -3` 分支已刷新，移植时核对该分支不要被重构丢掉；若 pong 路径断了，150 s 会变成误重连的源头（见 §4 的误重连观测）。
- **风险**：① 隧道开着时 M6 的 65 s 判定先触发，150 s 是隧道未开/已关时的兜底，两条路径的 `end_reason` 不同（M6 是 `"no response from VM (tunnel keepalive)"`），不要合并成一条；② `last_ws_ping_us` 必须在每回合用 `now_us` 的同一快照更新，不要在 ping 成功后另取一次 `esp_timer_get_time()`（上游用 `now_us`，避免两次取时之间的漂移让节流失效）。
- **回退**：把 ping 条件改回 `if (!did)` + TX 空闲 ≥60 s，删 `RX_IDLE_TIMEOUT_MS` 与 `last_ws_ping_us`；回退后半开连接的老坑回来（只发不收时永不拆），须在提交信息里写明。

### 2.5 M5：DMA 卡住 30 秒拆会话

- **文件**：只改 `esp32/main/noise_tunnel.cpp`（`pump_tx()`、`maybe_reopen()`、`try_open_stream()`、`noise_tunnel_tick()` 相关静态量；`TUN_KEEPALIVE_TIMEOUT_US` 移到 `noise_tunnel_internal.h`，见 §2.6）。
- **函数/行为**：新增 `TUN_TX_STALL_TIMEOUT_US`（30 s）、`TUN_TX_BLOCKED_RECORD_US`（5 s）、`s_tx_stalled` / `s_tx_stalled_since_us` / `s_tx_stall_min_dma`（会话任务独占）。`pump_tx()`：队列非空且 DMA 门连续关 30 s 无进展 → 记 `tunnel_tx_blocked`、返回 `-1`（会话循环拆会话）；任意一次成功发送、队列排空、流重开都 `end_tx_stall()`（≥5 s 的才记一条历史）。`maybe_reopen()`：DMA 门关着时不消耗重开限速、只累计 `s_outage_dma_wait_us`，连续被门控满 30 s 返回 false（拆会话）。**DMA 30 秒拆会话前核对隧道 TX 生产者**：`pump_tx` 返回 `-1` 时，会话循环是 `error = true; break;`，`s_work_q` 里未发出的 batch 随 teardown 丢弃——实现前先确认生产者（`send_packet`，任意任务）不会因此阻塞或丢重要帧：上游的取舍是"释放 DMA 内存优先于保住队列"，隧道丢包由上层 TCP 重传兜底；若我方有生产者依赖"batch 必达"的假设（例如控制面借隧道发关键帧），须先在设计里写清、不要直接移植。
- **风险**：① `s_tx_stall_min_dma` 是阻塞期间的最小值，`end_tx_stall` 后必须清零（随 `s_tx_stalled = false`），否则下一次阻塞会沿用上一次的最小值、历史失真；② `maybe_reopen` 的 DMA 门控查两道门（`noise_tx_has_dma_headroom` 与 `noise_tx_has_contiguous_dma_headroom`），只查一道会漏掉"总量够、连续块不够"的卡住形态。
- **回退**：删停滞计时与 `maybe_reopen` 的门控、把 `maybe_reopen` 的 `false` 返回点改回 `return true`；回退后 DMA 卡住的老坑回来（隧道外发队列永远发不出去、控制面还活着、会话永远不拆）。

### 2.6 M6：65 秒隧道超时分流 + 探活 WS ping

- **文件**：`esp32/main/noise_tunnel.cpp`（`s_unanswered_ping_us` / `s_timeout_ping_us` / `s_last_rx_s` / `note_tunnel_rx()` / `noise_tunnel_unanswered_ping_us()` / `noise_tunnel_take_unanswered_timeout()` / `noise_tunnel_rx_idle_s()`）、`esp32/main/noise_tunnel_internal.h`（`TUN_KEEPALIVE_TIMEOUT_US` 从 `noise_tunnel.cpp` 移过来；新增 `TUN_PING_PROBE_US` 5 s、`TUN_PING_ANSWER_US` 15 s；新增三个函数声明）、`esp32/main/noise_control.cpp`（会话循环 `tick()` 之后取 `take_unanswered_timeout()` 分流；ping 条件里加 `tun_probe`）。
- **函数/行为**：`note_tunnel_rx(now)`：任何入站隧道帧（pong 或数据）或流打开都刷新 `s_last_rx_us` 与 `s_last_rx_s`、清零 `s_unanswered_ping_us`。`tick()` 的 ping 发送点：只在无未答复 ping 时记 `s_unanswered_ping_us = now`。`tick()` 的 65 s 超时点：按 M6 规则决定 `s_timeout_ping_us` 存 ping 时刻还是 0。会话循环：`take_unanswered_timeout()` 取到非 0 且 `last_rx_us < unanswered_ping_us` → 重连（M11 的 `end_reason` / `went_silent`）；否则只重开隧道。ping 条件新增 `tun_probe`：`tun_ping_us` 非 0 且未答复 ≥5 s 且 `last_rx_us < tun_ping_us` 且 `last_ws_ping_us < tun_ping_us` → 补发一个 WS ping。
- **风险**：① `take_unanswered_timeout()` 是一次性取走（取后清零），会话循环每回合只调一次、放在 `tick()` 之后；调两次会把第二次的读成 0、漏掉重连判定；② `s_last_rx_s` 的 32 位 atomic 是给 `device.health` 的 `rx_idle_seconds` 跨任务读的（M8），不要把它和 `s_last_rx_us`（64 位、会话任务独占）混用；③ `TUN_KEEPALIVE_TIMEOUT_US` 移头文件后，确认 `noise_tunnel.cpp` 内不再有同名局部 `#define`（重复定义在 `-Werror` 下会编译失败）。
- **回退**：删分流与 `tun_probe`、把 `tick()` 超时点改回只标 down；回退后隧道后端挂与整条连接死不再区分，统一走重开隧道（重连要等 M4 的 150 s 兜底）。

### 2.7 M7：服务端 Reset 原因记历史

- **文件**：`esp32/main/noise_control.cpp`（新增 `RESET_REASON_MAX` 40 与 `copy_reset_reason()`；Reset 分支与拒开分支改调 `on_server_drop`）、`esp32/main/noise_tunnel.cpp`（新增 `noise_tunnel_on_server_drop()` 与 `s_outage_last_failure[TUNNEL_FAILURE_MAX]`）、`esp32/main/noise_tunnel_internal.h`（新增 `TUNNEL_FAILURE_MAX` 72 与 `on_server_drop` 声明）。
- **函数/行为**：`copy_reset_reason(StringView reason, char *out, size_t out_len)`：只留可打印 ASCII（`0x20–0x7e`，其余变 `?`），截到 `RESET_REASON_MAX`（40，含 NUL）。Reset 分支：`why` 拼成 `"server reset stream, code=%d: %s"`（无 reason 时不带冒号部分），调 `noise_tunnel_on_server_drop(why)`；拒开分支：`why` 为 `"server rejected open: %d"`，同调。`on_server_drop`：标隧道 down（`s_connected = false`），记 `tunnel_down`（`why` 进 `s_outage_last_failure`，供恢复时汇总进 `tunnel_up` 的 `"back after %us: %u reopens, %us DMA wait, last fail: %s"`）。`s_needs_reset` 不置位（M3 规则）。
- **风险**：① `why` 缓冲 `TUNNEL_FAILURE_MAX`（72）要装得下前缀 `"server reset stream, code=N: "` + 40 字节 reason——上游算过（最长 reason 36 字节 + 前缀），照搬尺寸不要缩；② `on_session_down()` 仍保留给会话结束（`why` 为空），不要把会话 teardown 处的调用也改成 `on_server_drop`——两者语义不同，改错会让每次会话结束都多记一条带空原因的 `tunnel_down`。
- **回退**：把两处调用改回 `noise_tunnel_on_session_down()`、删 `copy_reset_reason` 与 `on_server_drop`；回退后服务端原因只剩日志，不进历史。

### 2.8 M8：`device.health` 加 tunnel 与历史

- **文件**：`esp32/main/noise_control.cpp`（`send_device_health` 改导出、`tunnel` 对象、`connectivity_events`、注册描述）、`esp32/main/noise_control.h`（新增 `noise_ctrl_device_health_json(bool for_bug_report)` 声明与注释）、`esp32/main/noise_tunnel.cpp`（新增 `noise_tunnel_rx_idle_s()`，见 §2.6）。
- **函数/行为**：新 `noise_ctrl_device_health_json(bool for_bug_report)`：在现有 health 对象上加 `tunnel_connected` 与 `tunnel` 对象（`rx_idle_seconds` 仅当 `noise_tunnel_rx_idle_s() != UINT32_MAX` 时写；五个计数器照抄 `tunnel_netif_get_stats()`）；`link_events_add_json(health, "connectivity_events", for_bug_report ? 128 : 40)`；SSID 只在 `!for_bug_report` 时写。原静态 `send_device_health(session_generation, request_id)` 变薄壳（取对象 → 序列化 → `queue_result`，其余不动）。`link.register` 里 `device.health` 的命令描述改写，点明含 `connectivity_events`。头文件 include：`noise_control.cpp` 补 `#include "noise_tunnel.h"`、`#include "tunnel_netif.h"`、`#include "link_events.h"`、`#include "link_probe.h"`（上游 diff 的头部改动，照搬）。
- **风险**：① 我方 `send_device_health` 是静态内联版（`:1692`，无 tunnel、无历史），改导出函数时先确认没有别的同名静态函数、且 `handle_invoke_request` 里 `:1825` 的调用点签名不变；② `device.health` 的响应现在可能变大（40 条事件约 10–17 KB），`queue_result` / 控制帧分片路径（`CTRL_BODY_CHUNK_MAX`）要能装下——host 测试只验形状不验分片，实机验收时须发一次 `device.health` 看完整回包（见 §4）；③ `rx_idle_seconds` 省略与写 0 的区别：从未开过流是"无数据"不是"0 秒"，不要写 0 误导读数的人。
- **回退**：把 `noise_ctrl_device_health_json` 改回静态 `send_device_health`、删 `tunnel` 对象与 `connectivity_events` 两处；M9 的 health 内嵌须同步 revert（M9 依赖本项的导出函数）。

### 2.9 M9：`bug.report` 内嵌 health 与超限降级

- **文件**：`esp32/main/bug_report.c`（快照删隧道字段、`build_report_json` 改嵌 health、新增 `drop_oldest_events()` 与超限循环）、`esp32/main/bug_report.h`（`BUG_REPORT_MAX_BYTES` 注释补一句超限降级说明，值不动）。
- **函数/行为**：快照结构删 `tunnel_connected` / `tunnel_stats_t tunnel` 字段与 `collect_snapshot` 里的两行采集；`build_report_json` 删顶层 `"tunnel"` 对象，改为 `noise_ctrl_device_health_json(true)` 的返回值作 `"device_health"`（返回 NULL 则整段缺省、报告其余部分照常出）。序列化后若 `report_len > 32 KiB`，循环 `drop_oldest_events(root, report_len - BUG_REPORT_MAX_BYTES)` 删 `connectivity_events` 最旧一条（按"字符串长 + 3"估算），直到 fits 或删空；日志尾全程保留。`bug_report.c` 删掉 `#include "noise_tunnel.h"` / `#include "tunnel_netif.h"`（上游 diff 的头部改动）。
- **风险**：① `drop_oldest_events` 依赖 `device_health` / `connectivity_events` 节点存在——health 缺省时 `cJSON_GetObjectItem` 取不到、返回 false 即停，不要在循环外假设节点存在；② 删顶层 `"tunnel"` 对象是**面向 Muse 服务端的字段变化**，`bug.report` 的消费方（服务端解析、support 流程）若按旧字段读会读不到隧道计数器——新位置是 `device_health.tunnel`，移植后须在提交信息里写明字段搬家，不要静默改；③ 我方 `bug_report.c` 目前超限是整份丢弃，改循环降级时注意 `secure_zero` / `cJSON_free` 的旧指针处理（上游是先 `secure_zero` 再 free 再重新 `PrintUnformatted`，照搬顺序）。
- **回退**：恢复快照里的隧道字段与顶层 `"tunnel"` 对象、删 health 内嵌与 `drop_oldest_events`；回退后超限仍整份丢弃（老行为）。

### 2.10 M10：Wi-Fi 变迁记历史

- **文件**：只改 `esp32/main/wifi_mgr.c`（`event_handler` 两处 + 两处 include + 两个静态量）。
- **函数/行为**：新增 `s_has_ip`（初值 false）、`s_ip_lost_us`（初值 0）；`WIFI_EVENT_STA_DISCONNECTED` 分支：仅当 `s_has_ip` 时置 false、记 `s_ip_lost_us = esp_timer_get_time()`、记 `wifi_down`（`"esp-idf reason=%u rssi=%d"`）；`IP_EVENT_STA_GOT_IP` 分支：仅当 `!s_has_ip` 时置 true，`s_ip_lost_us` 非 0 则记 `"got IP after %llus without Wi-Fi"`，否则记 `"got IP"`。新增 `#include "esp_timer.h"` 与 `#include "link_events.h"`。重连退避（`s_retry` / `s_reconnect_backoff_ms`）与事件组位操作一字不动。
- **风险**：`event_handler` 跑在 ESP-IDF 事件任务上，调 `link_events_record` 合法（禁 ISR 不禁任务），但不要在这里加耗时操作；`rssi` 在 `ev` 为空时取 0（上游用 `ev ? ev->rssi : 0` 的三元写法，照搬）。
- **回退**：删两个静态量、两处记录、两处 include；其余 Wi-Fi 逻辑不受影响。

### 2.11 M11：会话 up/down、失败汇总、`went_silent` 探针

- **文件**：只改 `esp32/main/noise_control.cpp`（`run_session()` 内 + 文件级三个静态量）。
- **函数/行为**：`run_session()` 顶部（`ws_buf` 等指针声明之旁）加 `end_reason`（`"stopped"`）、`went_silent`（false）、`session_up_us`（0）；文件级加 `s_failed_attempts`（0）、`s_last_attempt_failure`（`""`）、`s_session_down_us`（0）与 `note_attempt_failed(reason)`（计数 +1、记最后一条原因）。建立前失败（TLS init/connect、WS upgrade 含 auth 拒绝）直接 `return` 前调 `note_attempt_failed`；建立后失败走 `goto cleanup`，`cleanup:` 处若 `session_up_us == 0` 补记一次 `note_attempt_failed(end_reason)`。`s_connected = true` 后记 `session_up`（有失败尝试时带 `"established after %us: %u failed attempts, last: %s"`，随后清零计数）；teardown 处（`s_connected = false` 之后）记 `session_down`（`"%s after %us up, rx idle %us"`）、置 `s_session_down_us`；仅当 `went_silent` 时调 `link_probe_on_stall()`。`end_reason` 的 19 处赋值逐项落点见 M11（TLS/握手/注册/收发/隧道各分支），**声明位置避开 `goto cleanup` 越过初始化**：三者必须放在 `run_session()` 顶部、所有 `goto cleanup` 目标之前（我方 `goto cleanup` 有多处：`:2129`、`:2138`、`:2158`、`:2186`、`:2197` 等），上游放在函数开头，照搬该位置；不要把声明挪到会话循环附近，否则 C++ 报 jump-over-initialization 编译失败。
- **风险**：① `note_attempt_failed` 的两种路径（直接 return 前 / cleanup 处）互斥是关键不变量：建立前失败走 return、建立后失败走 goto，若某条失败路径既 return 又经过 cleanup（我方 `run_session` 有多处 early-return，要逐一核对），会 double-count；② `session_up` 的耗时自 `s_session_down_us` 起算，首次启动时 `s_session_down_us` 为 0、耗时会算成自开机起——上游同样如此（首次 up 时 `s_failed_attempts` 为 0、走 `"established"` 分支，不读耗时），不要"修"它；③ `s_last_attempt_failure` 存的是字符串字面量指针（`const char *`，指向静态串），不要传栈上缓冲。
- **回退**：删三个局部量、三个文件级量与全部 `note_attempt_failed` / `link_events_record` 调用；回退后会话失败只剩日志，无汇总、无探针。

### 2.12 M12：测试移植

- **文件**：新建 `esp32/tests/link_events_harness.c`、`esp32/tests/test_link_events.py`；改 `esp32/tests/link_noise_tunnel_harness.cpp`、`esp32/tests/link_noise_transport_harness.cpp`、`esp32/tests/test_link_noise_tunnel.py`、`esp32/tests/test_link_transport_contract.py`、`esp32/tests/test_link_bug_report.py`、`esp32/tests/link_bug_report_harness.c`；`esp32/tests/link_fakes/` 的 `cJSON.c` / `cJSON.h` / `esp_timer.h` 有小幅改动（上游 diff 内含，照搬——改的是 fake 的能力，不是产品代码）。
- **函数/行为**：① `test_link_events.py`：host 编译 `link_events.c` + fakes（`-DLINK_FAKE_CUSTOM_HEAP_CAPS=1 -DLINK_FAKE_CUSTOM_TIMER=1`，ASan/UBSan），跑环的写入/覆盖/取最新 40 条与格式化。② 隧道 harness 加场景：DMA 门控重开（门关时 `maybe_reopen` 返回 true、不消耗重试间隔、`stream_ops` 无新增；门开后先 Reset 再 open，`"ORO"`；服务端已丢流时裸 open `"OO"`；ping 发送失败后重开也要 Reset；重开后被拒则下一次裸 open）、TX 停滞（阻塞 5 s 记一条、恢复后 `"resumed"`；满 30 s `pump_tx` 返回 -1）。③ transport harness 加 `test_rx_idle` 与 `test_dead_connection`：前者验 60 s 发 ping、150 s 拆会话（`end_reason "no response from VM"`、`went_silent`、日志 `"nothing received for 150s; reconnecting"`）、TX 活跃时仍按 RX 空闲 ping（60 s 最多一次）、入站不断则一直活；后者验隧道超时分流的五种情形——全程静默则 65 s 重连（`end_reason "no response from VM (tunnel keepalive)"`、无 reopen，`stream_ops == "O"`）、隧道 ping 未答复 5 s 后补发一个 WS ping、WS pong 到了则只重开隧道（`"ORO"`）、ping 发出太晚（距超时不足 15 s）则只重开隧道、ping 全被 DMA 门挡住则不重连、等重开门控满 30 s 后以 `"tunnel reopen failed"` 拆会话（且发生在 150 s 之前、`went_silent` 为 false）。④ `test_link_noise_tunnel.py` 注册 `test_silent_session_pings_then_reconnects`（`rx_idle`）与 `test_dead_connection_reconnects_at_tunnel_timeout`（`dead_connection`），并在切片常量里补 `RX_IDLE_TIMEOUT_MS`（上游 diff 的 `test_link_noise_tunnel.py:67-70`）。⑤ `test_link_transport_contract.py` 把 `maybe_reopen` 契约断言从 `void` 改 `bool`（两处字符串断言，见 M12）。⑥ bug report harness：删直采隧道计数器的 fake，改用 `noise_ctrl_device_health_json` 的 fake（128 条最长事件 `e000`–`e127`），`test_link_bug_report.py` 补三点断言（M9）。
- **风险**：**harness 切片锚点保持**——`test_link_noise_tunnel.py:39-90` 用字符串锚点（`control.index("static char s_register_req_id[")`、`control.index("        // Once registration is acked")`、`control.index("        // TX: keep exactly one control message")`、`control.index("cleanup:\n")` 等）从 `noise_control.cpp` 切片；M4/M6/M11 在会话循环里加代码时，**不要改动这些锚点行的文字、不要删锚点之间的结构**，否则 harness 切片失败（不是产品代码错，是测试取不到代码）。新增代码尽量插在锚点区间内部，不要跨锚点移动代码块。⑦ `link_fakes` 的改动只动 fake，不要顺手"修"产品代码去迁就 fake。
- **回退**：删新增的两个测试文件、把改过的 harness/py 文件 revert；产品代码不受影响（测试与产品是分开提交的，上游也是 25 文件里测试占 9 个，见 diff 文件清单）。

## 3. 不做项（明确拒绝，勿顺手做）

- **`88b04a3` 删 `device.ota`**：那是上游另一笔提交，不属于 `481743f` 的 25 文件 diff；删 `device.ota` 等于砍掉 Muse 下发升级的唯一入口，且我方分支已用 `HOMEHUB_ADVERTISE_OTA` 门控默认不广播它（`noise_control.cpp:1576` 注释：云端曾拿官方 Home Link 固件回应广播，芯片不匹配、会覆盖本构建），删除的收益与风险须另行决策。处置见 §6，本次不动。
- **链路换段**：局域网 → 腾讯云 → CF relay.example.com → 美国 VPS → Muse 服务器，任何一段的更换、绕开、新增中转都不在本次范围。逐跳实测结果还没回来，没有数据支撑换段决策；本次只做固件侧自愈与诊断，让 history 先把"哪一跳"问清楚。
- **屏上显示历史**：不把 `connectivity_events` 做到屏上（我方树里没有 `muse_ui.c`，屏显走 `app.c` 的 `ui_set_status`）。360 px 圆屏放不下历史列表，且历史是给 Muse 服务端读的（`device.health` / `bug.report`），不是给人盯着看的；屏上只保留现有状态提示。
- **阈值改 Kconfig 可调**：12 个阈值（§1 总览表）全部照搬上游原值，不包 Kconfig。理由：调参须有实机数据再定，现在没有；Kconfig 化会让本分支与上游的数值语义分叉，后续同步要逐项对值；真要调时先在实机上量出误触发/漏触发的证据，再单项改常量。
- **探针改测 Muse 端点 / 加 DNS**：`link_probe` 只测 `1.1.1.1` / `8.8.8.8:443`、不做 DNS、不发数据。改测 Muse 端点（`relay.example.com` 或 Muse 域名）会把"互联网通不通"与"Muse 链路通不通"两个问题混成一个，探针就失去分流意义；加 DNS 则引入新的失败模式（DNS 劫持/超时会被误读为断网）。局域网本来就直连不了 CF，测 CF 端点会恒读失败。见 §5。
- **WS ping 改短**：`KEEPALIVE_INTERVAL_MS` 60 s 与 `RX_IDLE_TIMEOUT_MS` 150 s 不动。60 s 是中继/中间盒空闲定时器的常见量级，改短只会增加流量与功耗，不解决半开问题；150 s 是"两声未答复的 keepalive 之后"的量级，改短会把正常抖动误判成死连接。两者都要有实机误触发数据才动。

## 4. 实机验收清单（全部结论标「须实机量测」）

host 测试通过 ≠ 实机通过。下表每一项的"通过标准"都以实机读数为准；没有实机读数的项写"未测"，不要写"预计通过"。

| # | 场景 | 复现路径 | 观测点 | 通过标准（须实机量测） |
|---|---|---|---|---|
| A1 | 短断网（WAN 断 30 s 内恢复） | 会话正常时，拔路由器 WAN 线，30 s 内插回 | `device.health` 的 `connectivity_events`；会话是否重连 | 隧道 keepalive 能自愈或只重开隧道，不拆会话；history 里有 `tunnel_down` / `tunnel_up` 一对（须实机量测） |
| A2 | 长断网（WAN 断 3 分钟） | 同上，断 180 s 再恢复 | 同上 + 从断网到 `session_up` 的耗时 | 65 s（隧道开着）或 150 s（隧道未开）内拆会话重连；重连后 `session_up` 带 `"failed attempts"` 汇总（须实机量测） |
| A3 | Wi-Fi 断（关 AP / 路由器断电） | 关 AP 60 s 再开 | history 里 `wifi_down` / `wifi_up`；探针结果 | `wifi_down` 带 `esp-idf reason`，`wifi_up` 带 `"got IP after %us without Wi-Fi"`；`stall_probe` 为 `"wifi down"`（须实机量测） |
| A4 | 半开连接（WAN 断后 NAT 映射丢、不断 Wi-Fi） | 路由器 WAN 断开后立刻恢复 WAN（LAN 不断），使 TCP 半开 | 是否在 150 s 内重连；`end_reason` 是 `"no response from VM"` 还是 `"no response from VM (tunnel keepalive)"` | 隧道开着时走 M6 在 65 s 分流重连，隧道未开时走 M4 在 150 s 重连；不出现"几分钟无响应也不重连"（须实机量测） |
| A5 | 隧道后端挂、控制面活 | 无可靠家庭复现手段（需服务端配合拒开/Reset 隧道流） | `tunnel_down` 的 `why` 是否为 `"server rejected open: %d"` / `"server reset stream, code=%d: …"`；重开是否一次成功 | 服务端 Reset 后重开不再报 "duplicate id"，且不拆会话（须实机量测；若无服务端配合，本项记"未测"） |
| A6 | DMA 卡住 | 无可靠家庭复现手段（需持续 DMA 压力） | history 里 `tunnel_tx_blocked` 是否出现；误触发次数 | 正常使用下不出现 `tunnel_tx_blocked` 拆会话；若出现，核对当时是否真在 DMA 压力下（须实机量测；无复现手段时记"未测"，不要编造） |
| A7 | `device.health` 回包完整 | 会话正常时发一次 `device.health` | `tunnel` 对象五计数器 + `rx_idle_seconds` + 最新 40 条 `connectivity_events` 是否全在；回包是否被分片截断 | 字段齐全、无截断（须实机量测；§2.8 风险②） |
| A8 | `bug.report` 超限降级 | 先攒满 128 条事件（多次断网/重连），再发 `bug.report` | 报告总长 ≤32 KiB；最新事件在、最旧事件被丢；日志尾完整；无 `network_ssid` | 满足 M9 的三点（须实机量测） |
| A9 | 误重连观测（静置） | 正常网络下静置 24 h，不操作 | `session_down` 的次数与 `end_reason` 分布 | 24 h 内无 `went_silent` 误触发（`no response from VM` 出现即为误触发，需查原因）；正常 keepalive 下 `session_up` 只有开机一条（须实机量测） |
| A10 | 重连耗时 | A2 的断网恢复后计时 | 从 WAN 恢复到 `session_up`、到隧道恢复（`tunnel_up`）的两段时间 | 两段耗时如实记录（须实机量测）；上游未给数字，本设计不承诺任何秒数 |

验收记录格式：每项写"日期 + 场景 + 观测到的 history 原文（`"<N>s ago <kind>: <detail>"` 逐条抄）+ 结论（通过/未通过/未测）"。history 在重启后丢失，验收期间不要重启设备；需要留档时先发 `device.health` / `bug.report` 把 history 抄出来再重启。

## 5. 探针意义与局限（在用户链路下）

用户链路是：局域网 → 腾讯云 `example.com` → 反代 → CF 上的 `relay.example.com`（局域网无法直连 CF）→ 美国 VPS → Muse 服务器。探针测的不是这条链路，测的是"设备经局域网能不能到达公共互联网"。分流逻辑如下：

- `stall_probe: internet ok` + 会话静默 → 故障在局域网之外：腾讯云反代、CF、美国 VPS、Muse 服务器四段之一（探针区分不了是哪一段；逐跳定位仍须等三台机器上的 curl 实测结果）。此时重连会自己恢复或继续失败，history 里的 `session_up` 汇总（失败次数与最后一条原因）是下一步的证据。
- `stall_probe: internet unreachable (1.1.1.1 …, 8.8.8.8 …)` → 故障在局域网/宽带侧：设备连公共互联网都到不了，重连注定失败，等网络恢复。不要在这种情况下反复手动重启设备——重连退避已在跑，重启只会丢掉 history。
- `stall_probe: wifi down` → Wi-Fi 本身断了（AP/路由器问题），与远端链路无关。
- 局限四条：① 探针只在 `went_silent` 时触发一次，不是周期巡检——两次静默之间没有探针数据，不要拿"没有探针记录"当成"网络一直正常"；② 探针不测 DNS、不测 Muse 端点、不测 CF——它回答不了"腾讯云反代是不是挂了"；③ `refused` 也算到达：对端主机活着但端口拒绝，仍记 `"internet ok"`——这是有意的（到没到达主机才是问题，端口状态与本设备无关）；④ 探针结果写进 history 时带的是触发时刻的结论，重连成功后它仍是旧结论，读的时候按 `"<N>s ago"` 对时间，不要当成当前状态。

## 6. 后续同步注意

- **`88b04a3`（删 `device.ota`）的处置**：只删 `esp32/main/noise_control.cpp` 里的两处——`link.register` 的 `device.ota` 命令注册（我方 `:1591` 起，`HOMEHUB_ADVERTISE_OTA` 门控内）与 `handle_invoke_request` 里的 `device.ota` 分支（我方 `:1793` 起，调 `ota_start` 的那段）。**不动**：配网期动作（`app.c:1238-1244` 与 `:1592-1594` 的 BLE 配网 OTA，直接调 `ota_start`，不走 `device.ota`）、`ota.c` / `ota_github.c`、分区表、回滚逻辑（`app.c:210`、`:2839` 的 `PENDING_VERIFY` 回滚）。**删之前须先确认是否有人在用 Muse 下发 `device.ota` 升级**：若用户或 Muse 服务端还在用这条路下发固件，删了就断了升级通道，须用户亲口确认不用再删。补充两点实情：① 我方分支的 `device.ota` 已默认不广播（`HOMEHUB_ADVERTISE_OTA` 门控，`:1576` 注释写明云端曾拿官方 Home Link 固件回应广播、芯片不匹配），所以"删"与"不广播"在效果上只差在"服务端硬调 `device.ota` 时是否还受理"；② 本设计所用的上游克隆是浅克隆（`481743f`  detach，本地仅 3 笔提交），`88b04a3` 不在其中、未逐行核对——动手前先取上游原文核对它到底删了哪几处，再按本节的"只删两处"落点执行，不要凭本节文字直接删。
- **同步上游 `#372` 的后续**：`481743f` 是 PR #372 的合入提交，后续上游若有跟进修补（例如 `drop_oldest_events` 的字节估算、探针任务栈、DMA 门控阈值），同步时只取与本移植同一主题的改动，不要把不相关的上游提交一起合进来。
- **阈值是常量、不是配置**：后续任何调参（§1 总览表）都先有实机数据（§4 的 A9 误触发观测、A2/A4 的重连耗时），再单项改常量并写明数据来源；不要改成 Kconfig（见 §3）。
- **基点不变**：本移植基点是 main `803ca78`（1.5.3 正式版）；内存优化分支 `feat/memopt-20261117` 未合 main，不以它为基点、不引用它的改动。若 `memopt` 先合 main，移植前重核 `noise_control.cpp` / `noise_tunnel.cpp` 的行号与切片锚点（§2.12 的锚点行最容易被重构带偏）。
- **不要动链路**：后续同步只动固件，链路换段另行决策（见 §0、§3）；history 攒够证据（`stall_probe` 与 `session_up` 汇总）后再议。

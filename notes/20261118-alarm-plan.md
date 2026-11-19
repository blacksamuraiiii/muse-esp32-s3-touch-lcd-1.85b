# 闹钟设置项设计（20261118-alarm-plan）

> 状态：**已定版（用户 2026-11-18 拍板 7 点，见 §10）**，实现见 §11；分支 `feat/alarm-20261118`（本地，未推）。
> 基点 main `803ca78`（1.5.3 正式版）。

> **合入 1.5.4a 时用户改的口径（2026-11-18，实机看过第一版之后）**：
> ① 响闹页的「停止」按钮**不再等 6 秒面板稳定期**（`muse_input_alarm_stop()` 去掉 grace 检查，见 §3.2/§11）；BOOT 与触摸的 6 秒宽限不变——即响闹最初 6 秒内按钮点按立即停，BOOT/触摸仍须等宽限过后；
> ② 闹钟时间设置页的两个滚轮**去掉外框**。
>
> **F-A1 修补（2026-11-19，1.5.4a 评审）**：SNTP 回调不重响同日，但正向跳过目标仍丢当天（见 §1.5/§3.5）。

## 0. 一句话结论

**闹钟不是从零开始——1.5.3 里已有一套端到端的闹钟实现。** v1 的范围是补齐它的短板，不是重做：

| 短板 | 现状 | v1 做法 |
|---|---|---|
| 分钟只能 ±10（07:05 设不出来） | 点行左半降、右半升，分钟 ±10 | 新增时间设置页，双滚轮（时/分） |
| 响闹没有专属页面 | 只有脸部字幕 `ALARM 07:00 / PRESS TO STOP` | 新增响闹页（大字时间 + 只留停止按钮） |
| 没有小睡 | 无 | **不要小睡（定版，见 §10-1/6）** |
| 没有试响 | 无 | **不要试响（定版，见 §10-4）** |
| SNTP 校时后不重 arm | 校时只写芯片，不重 arm | SNTP 同步后重 arm（小修补） |

不碰：入口（SETTINGS → Alarm）、重复规则（4 档）、存储位置（settings NVS）、触发机制（RTC 芯片闹钟）、金币经济、唤醒源红线。响闹音为 B 候选双声蜂鸣（定版，见 §10-2），自动停由 60 秒改为 30 秒（定版，见 §10-5）。

## 1. 现状（已核实，1.5.3，main `803ca78`）

### 1.1 设置页

`esp32/components/muse/muse_settings_ui.c`（1955 行），`muse_settings_ui_build()` `:1908`。

- 层级 2 级：SETTINGS 主页 → 子页。主页 `build_home()` `:1813`，9 行：Wi-Fi、Muse、TTS、Bluetooth、Sound、Sleep、**Alarm**（`:1823`，`LV_SYMBOL_BELL`）、Battery，另 Update（仅 OTA 构建）、Power off。Alarm 行右侧显示 `HH:MM` 或 `Off`（tick `:1872-1879`）。
- Alarm 子页 `build_alarm_page()` `:1581`，`page(tile, "ALARM", true, …)`，4 项 + 一行提示：
  - 开关行 `switch_row("Alarm", …)` `:1585`，开关 60×32；
  - Hour 行 `:1586`，点左半 −1、右半 +1（`tap_direction()` `:1526`，SLEEP 页同款模式）；
  - Minute 行 `:1587`，**±10**，55+10 回 0 而非 5（`:1544-1550`，注释引 `docs/20261023-待办.md §2.3`）；
  - Days 行 `:1588`，在 Once / Every day / Workdays / Weekends 间循环（掩码 `0x00/0x7F/0x3E/0x41`，`:1556-1557`；非常规掩码显示 "Custom"）；
  - 提示分两行：动态 hint `note(list, "")` `:1589`，闲时为空、响铃时 `tick_alarm()` `:1595` 改写为 "Ringing now - any key stops it"；其下还有一段静态说明（`:1590-1592`，点行左半降右半升、芯片记时间与星期、灭屏也能响）。
- 子页不常驻：`show()` `:396` 切页时把离开的非主页子页 `drop()` 删掉，首次进才 build（懒建）。
- 子页禁 tile 右滑：`show()` `:408` 仅主页时开 swipe；互动子页同款机制在 `muse_interact_ui.c:78-86`。
- 控件库（同一文件）：`row()` 58px 卡片行 `:278`、`switch_row()` `:315`、`slider()` `:343`、`button()` `:330`、`note()` `:161`。列表宽 `LIST_W 330`、顶 `LIST_TOP 84`（`:52-54`）、行高 `ROW_H 58`、仅纵向滚动、无滚动条、圆屏底部留白 110。
- 入口：三 tile 结构 `muse_ui.c:961-982`——互动 (0,0)、对话脸 (1,0)、设置 (2,0)，设置只允许从脸向左滑进。

### 1.2 触发机制（RTC 芯片拥有触发权）

`esp32/components/muse/muse_input.c:235-495`（头注释 `:235-243`）与 `muse_rtc.{h,c}`。

- `alarm_arm_next()` `:267`：按设置时分 + days 掩码算下一次目标（今天已过则往后找，最多 7 天；`mktime` 正规化日期并填 `tm_wday`，搜索与星期由构造保证一致）。
- 每天走 `muse_rtc_arm_wakeup_in()`（2～86399 秒），带星期过滤或 >86399 秒走 `muse_rtc_arm_wakeup_weekly()`（`:518`，2 秒～7 天，直接写 5 字节 payload、星期参与比较、日屏蔽）。芯片只有一组闹钟寄存器（秒/分/时/日/星期，`0x0B–0x0F`），**精度到秒**；`set_alarm()` 把日和星期写 `0x80`（屏蔽），只比秒/分/时。
- `muse_rtc_disarm()` `:567`：关 AIE 并清 AF，否则 AF 锁存会拉住 INT 导致一睡即醒。
- 设置变更经 `MUSE_SETTING_ALARM` 通知（`muse_settings.h:48`），`muse_app.c:72` 监听回调调 `muse_input_alarm_sync()` 重新 arm/disarm。开关 off 时不留任何唤醒源（`muse_input.c:361-364`）。

### 1.3 响闹行为（已实现）

- 响 60 秒后自动停（`ALARM_RING_US`，`:243`）；每 2 秒一次上升 chirp（`ALARM_CHIRP_US`，`:246`）：`muse_voice_request_chirp()` → voice 任务 `muse_audio_chirp(1)`（`muse_voice.c:1078`），90ms、620→1180Hz 上升扫频、幅度 7000（`muse_audio.c:566` 起，`muse_audio_chirp_locked`）。
- `alarm_start()` `:354`：`set_asleep(false, "alarm")` 唤醒状态机（亮屏）、`muse_state_poke()`（响闹期间抑制自动睡）、字幕 `"ALARM %02d:%02d\nPRESS TO STOP"`。**没有专属响闹页**，只有脸部字幕。
- 停止：任意键（`ALARM_STOP_BITS` `:250-252`，含 BOOT/触摸/各方向键）。但有 6 秒防误触宽限（`ALARM_TOUCH_GRACE_US`，`:245`）：面板唤醒时 CST816S 会报一次幻触，BOOT/触摸按压须等面板稳定后才计入，**其他键（本板没有）立即计入**——即在本板上，响闹最初 6 秒内一切停止输入都无效，6 秒后 BOOT 或触摸可停（`input_task` `:925-946`，2026-10-23 实测：幻触约 4 秒到达）。
- 停后：once 闹钟自动关（`alarm_stop()` `:401-424` 调 `muse_settings_set_alarm_on(false)`），其余重 arm 下一次。响闹期间 talk 按压被吞掉（`:948-956`），停后 2 秒内同一次按压的释放不会漏进语音管线（`ALARM_QUIET_US`）。
- 清醒时 `alarm_poll()` `:405` 每 500ms 读一次芯片 AF（`ALARM_CHECK_US`），另有时钟到达 `s_alarm_target` 的兜底。
- 深睡唤醒后：`input_task()` 开头 `:893-915` 读唤醒原因 + `muse_rtc_alarm_pending()`，闹钟 on 且 AF 置位则 `alarm_start("boot")`，否则 `muse_input_alarm_sync()`。
- 控制台：`>alarm / alarm.off / alarm.test / alarm.stop / alarm.arm=N`（`muse_input.c:1463-1507`）。

### 1.4 存储

- 闹钟配置**不在宠物状态里**，在另一套 NVS：`esp32/components/muse/muse_settings.c:31`，命名空间 `"muse"`，key `alarm_h / alarm_m / alarm_on / alarm_days`（加载 `:157-161`，保存于各 setter `:308-346`）。默认值 `alarm_h=7, alarm_m=0, alarm_days=0x7F`（`:72-74`）；`alarm_on` 未在初始化表里显式列出，静态零初始化下默认为 off。
- 宠物状态是另一套：命名空间 `"pet_pub_stats"`、key `"state"`、单个 blob、版本 v6（`muse_pet_model.h:48`），迁移链 v2→v6 在 `muse_pet.c`。**闹钟不动宠物 blob**，因此不需要版本号与迁移（见 §7）。

### 1.5 时间

- 芯片存 UTC，系统时钟也存 UTC，本地时只在显示时由 `localtime_r()` + `CONFIG_MUSE_TIMEZONE` 换算（`muse_rtc.h:37-42`）。`muse_rtc_adopt()` `:275`（系统时钟 < 门槛 `SANE_EPOCH` 2025-01-01 UTC 且芯片时间有效时从芯片恢复）、`muse_rtc_push()` `:294`（系统时钟 → 芯片）。SNTP：`muse_time_start()` `:391`，`time_task` `:347` 5 秒轮询，Wi-Fi 连上后 `esp_netif_sntp_init`，同步回调后 `muse_rtc_push()` 写回芯片。
- **SNTP 跳变（P5 + F-A1，见 §3.5）**：SNTP 同步后重 arm（`muse_rtc.c` 的 `time_task`，只在真实同步回调里调一次）。重 arm 只保证**下一次 arm 的日期正确**：往回跳（如 07:05→06:55，07:00 已响过）不再响第二遍——响闹触发时记下的本地日期（`muse_input.c` 的 `s_alarm_last_rung_year`/`s_alarm_last_rung_yday`，内存静态量）让 `alarm_arm_next()` 跳过已响过的日期，再按 Days 掩码找下一个合法日；**正向跳过目标（如 06:59→07:01）仍丢当天那一响**，无法无中生有补响。已知边界：日期标记只放内存，重启后丢失——重启后若时间再往回调过重启前已响过的闹钟，可能重响一次；同日内把闹钟改到当天晚些时候（如已响过 07:00，08:00 改到 09:00）也会被跳过、改到下一个合法日，不是当天 09:00。

## 2. 参考实现 pocket-watch（MIT，同板）

仓库 `jamesblashill/pocket-watch`，HEAD `fd99f55`，MIT。克隆在 `/tmp/pocket-watch`（只读）。

### 2.1 它家闹钟极简

只支持 **1 个闹钟、每天重复、无星期/标签/小睡/自动停**。README 称 "alarms with volume and scheduling controls"（复数），实际代码只有 1 个闹钟、无星期控制，**以代码为准**。

| 模块 | 管什么 | 文件 |
|---|---|---|
| `app_alarm` | 闹钟调度与触发逻辑（与 UI 无关） | `main/app_alarm/alarm.{c,h}` |
| `app_timer` | 倒计时器（与闹钟完全独立） | `main/app_timer/timer.{c,h}` |
| `app_clock` | 全部 LVGL UI：表盘 + 闹钟/音量/计时器/亮度/主题/时间设置屏 | `main/app_clock/lvgl_*.c` |
| `app_rtc` | PCF85063 驱动封装（仅取/设时间） | `main/app_rtc/rtc.{c,h}` |
| `app_net` | Wi-Fi + SNTP 校时 | `main/app_net/wifi_time_sync.{c,h}` |

### 2.2 可直接借的（MIT，机制与我们板子同源）

- **1 秒 `esp_timer` 轮询 RTC 的触发骨架 + 注释写明的“不用 `lv_timer`”理由**（`alarm.c:140-171`）：屏灭时 LVGL worker 被 pause，`lv_timer` 不跑，只有 `esp_timer` 能在灭屏时继续触发。我们是芯片闹钟 + 500ms 轮询兜底，思路同源，骨架不用照搬，但“灭屏仍要触发”这一条是共同前提。
- **`s_armed` 防同分钟内重复触发模式**（`alarm.c:31-37,140-158`）：时钟离开闹钟分钟后把 `s_armed` 重新置 true。我们靠 `alarm_arm_next()` 单点重 arm 达到同样效果（头注释 `:239-243` 写明“重 arm 只在这里，响过的闹钟不会响两次”），机制不同，不用改。
- **NVS 存闹钟的极简模式**：命名空间 + u8 键 + 改动即 commit（`alarm.c:44-72`）。我们已是同款（`muse_settings.c` 的 `load_u8`/`save_u8`），不用改。
- **响闹时唤醒屏 + 响闹期间抑制空闲熄屏**（`alarm.c:116-121`；`lvgl_screen_power_button.c:192`）。我们已有 `set_asleep(false, "alarm")` + `muse_state_poke()`，同款，不用改。
- **音量滑杆（0–100）+ 试响按钮的 UI 模式**（`lvgl_alarm_volume.c`）；**防误触的 `LV_OBJ_FLAG_SCROLL_CHAIN` 去除套路**（多处）：滚轮/滑杆去掉该标志，防止点按时手指轻微滑动把 tileview 误滑走。我们的滚轮页必须照抄这一条（见 §3.1）。
- **播放期间才打开 codec、播完关闭以保 light sleep**（`bsp_board_extra.c:22-32,64-69`）。我们的 chirp 本来就是按需渲染、无常驻音频路径，同款，不用改。
- **RTC 硬件闹钟接口的存在性**（`pcf85063a.h:141-156`）：驱动已支持 `set_alarm`/`enable_alarm`，可作为日后深睡唤醒的备选——但它家自己没用、未经验证；**我们已经在用**（`muse_rtc_arm_wakeup_in/weekly`），这条对我们是已知项。

### 2.3 它家没有的（须我们新做）

- 多闹钟数据结构 + 重复规则（星期/工作日/单次）+ 标签：它家只有单闹钟三字段。——我们已有 4 档重复规则（§1.1），v1 不动。
- 小睡、自动停、响闹专属界面、手动设时间 UI：全部没有，需新做（小睡与响闹页见 §3；手动设系统时间与闹钟无关，不做）。
- 音频：它家是 SD 卡 WAV + `esp_audio_simple_player`，且 WAV 文件本身不在仓库里；我们走 chirp 合成音，音源与播放器都不同，只能借“音量可调、试响、播完关 codec”的思路，不能照搬实现。

### 2.4 不要抄的

- 手表式 tileview 导航整套（时钟居中、左状态屏、右设置链、计时器在上）、数字/模拟双表盘与主题系统、设备状态/电量/亮度屏——与闹钟功能无关。
- `app_net` 的 Wi-Fi 凭证与同步模块原样：含美东 DST 逻辑（`wifi_time_sync.c:51-66`），不适合我们（中国时区 + 我们的校时链路）。
- 屏灭/亮的 panel 拆解重建逻辑（`lvgl_screen_power_button.c:78-178`）：为 light sleep 的 QSPI/DMA workaround 而写，注释自认曾致死机、属 fragile 的板级特例；只借“灭屏仍跑 esp_timer”的思路，不抄实现。
- BOOT 长按 = 卸载 SD 卡：与闹钟无关。
- SNTP 跳变漏响：它家是分钟级精确相等比对，SNTP 一次跳过闹钟分钟当天就漏响；我们是芯片绝对秒数 arm，机理不同但同款缺口在 §1.5，v1 用重 arm 修（§3.5）。

## 3. v1 功能范围（推荐方案）

### 3.1 P1：时间设置页（新，子页第 3 级）

- 入口：SETTINGS → Alarm 子页 → 点“时间”行进。标题 "Set time"（设置页英文惯例，见 §4）。
- 内容：两个 `lv_roller`（`LV_ROLLER_MODE_INFINITE`），时 00–23、分 00–59，各宽 110px、可见 3 行、中间冒号分隔（pocket-watch 同款，`lvgl_alarm_settings.c`）。大数字用 **Montserrat 28**（现有先例：配对码 28、数学练习题干 28 再 `transform_scale 439/256` 放大；数字与 ":" 是拉丁字符，不受 CJK 只有 16px 的限制）。
- 保存语义（与 pocket-watch 不同，特意改）：滚轮滚动只改显示，**离开页面时一次性写 NVS + 重 arm**。pocket-watch 是每次 `VALUE_CHANGED` 立即写 NVS，我们的设置变更还会触发 `MUSE_SETTING_ALARM` → 重 arm，逐格写等于每滚一格就写一次 Flash + 重 arm 一次。离开时提交一次即可。
- 防误触：滚轮去掉 `LV_OBJ_FLAG_SCROLL_CHAIN`（pocket-watch 套路）；本页固定布局、**不滚动**（设置页列表默认纵向滚动，本页不用列表容器）。
- 退路：滚轮若实机手感差，退回点按行、分钟改 ±1（仍比 ±10 强），不改其他部分。

### 3.2 P2：响闹页（新，全屏覆盖层）

- 形态：**全屏覆盖层**（盖在当前 tile 之上），不是新 tile。理由：闹钟可以在任意 tile 下响（对话页、互动页、设置页、灭屏唤醒后），新 tile 要改 `muse_ui.c` 的 3-tile 写死结构（`:1474`）与 `update_chrome()` 页码逻辑；覆盖层不用动 tile 结构，关掉即回到响闹前停留的那一页。先例：掷筊 9 次用完的提示框就是模态覆盖层（压暗整页）。
- 内容：大字时间 `07:00`（Montserrat 28 + `transform_scale`，数学练习先例，约 48px）+ "每天 · 闹钟响了"（DIM，重复档不同则换成“一次/工作日/周末/自定义 · 闹钟响了”）+ **一个按钮：停止**（DANGER，居中 160×48）。无返回钮（响闹中不能返回），提示行 "按 BOOT 也可停止"。
- 出现时机：`alarm_start()` 被调时推入（清醒：poll 触发；轻睡：GPIO6 结束小睡后 poll 触发；深睡：boot 后 `alarm_start("boot")` 时推入）。`alarm_stop()` / 30 秒超时后撤掉。推入/撤掉不由 `alarm_start()` / `alarm_stop()` 直接调 LVGL（input 任务不在 LVGL 线程）：响闹页是 `lv_layer_top()` 上的全屏覆盖层，`muse_alarm_ui_tick()` 每帧在 LVGL 线程里读 `muse_input_alarm_ringing()`，响起就显示、停了就撤掉（见 §11）。
- 停止路径不变：按钮走同一条 `alarm_stop()`（新出口 `muse_input_alarm_stop()`，**按钮不等 6 秒宽限**，见 §11）；BOOT/触摸仍按现有 6 秒宽限逻辑（§1.3）——即响闹最初 6 秒内 BOOT/触摸按压无效（防幻触），6 秒后生效；按钮点按立即停。UI 上不额外提示宽限（不给用户增加概念），按钮始终画出来。
- 响闹期间不走 tile 右滑、不显示页面圆点（覆盖层盖住），与互动子页同款处理。

### 3.3 P3：小睡——已否决

**用户 2026-11-18 定版：不要小睡。** 本节原方案（点“小睡”停响 + 一次性重 arm +5 分钟、最多 3 次、计数只放内存）全部不做；不新增 `alarm_snooze` NVS key；响闹页不画小睡按钮（§3.2 只有一个停止按钮）。原方案的细节留档在此，不实现。

### 3.4 P4：小睡开关——已否决

**用户 2026-11-18 定版：不要小睡开关。** Alarm 子页不新增这一行（§3.6 只有 3 行）。

### 3.5 P5：SNTP 同步后重 arm（小修补）

- `time_task` 的 SNTP 同步回调在 `muse_rtc_push()` 写回芯片后，调一次 `muse_input_alarm_sync()`（响闹中不调——`muse_input_alarm_sync()` 本身在响闹时直接返回，`muse_input.c:356-358`）。它修的是**下一次 arm 的日期正确性**，不是补响当日（见 §1.5）。
- 真实保证（F-A1，2026-11-19 修补）：响闹触发时 `alarm_start()` 记下响闹的本地日期（`s_alarm_last_rung_year`/`s_alarm_last_rung_yday`，`muse_input.c` 内存静态量）；`alarm_arm_next()` 跳过已响过的日期，再按 Days 掩码找下一个合法日。往回调（如 07:05→06:55）不再当天响第二遍；once 闹钟响完已自动关，不走此路。**正向跳过目标（如 06:59→07:01）仍丢当天那一响**——重 arm 只能算出明天，无法补响。
- 已知边界：日期标记只放内存，重启后丢失（§1.5）；同日内改时间到当天晚些时候也会被跳过、改到下一个合法日。
- 约束：只在“时间真的被校准过”的回调里调，不要每 5 秒轮询都调（`time_task` 5 秒轮询一次，见 `muse_rtc.c:347`）。

### 3.6 v1 后的 Alarm 子页（3 行 + 提示）

| 行 | 内容 | 交互 |
|---|---|---|
| 1 | 开关 Alarm | `switch_row`，开/关（现有） |
| 2 | 时间 07:00 | 点进 P1 时间设置页（现有 Hour/Minute 两行合并为这一行） |
| 3 | 重复 每天 | 点按循环 Once / Every day / Workdays / Weekends（现有） |
| 提示 | 静态说明改为 "Tap Time to pick the time. The alarm rings 30 s, any key stops it."（效果图里折两行）；动态 hint 不变，闲时为空、响铃时 "Ringing now - any key stops it"（现有） |

Hour/Minute 两行合并为“时间”一行，是 P1 的一部分：两行点按 ±1/±10 的旧交互随 P1 退役（代码删 `on_alarm_hour`/`on_alarm_min`，保留 `tap_direction` 给 Sleep 页用——它是 SLEEP 页同款模式，`tap_direction()` 本身不删）。

## 4. 入口与页面结构

```
对话脸 (1,0) ──左滑──▶ SETTINGS 主页 ──点 Alarm 行──▶ Alarm 子页 ──点“时间”行──▶ 时间设置页
                                                                              （滚轮，离开即保存）
闹钟响 ──▶ 响闹页（全屏覆盖层，盖在当前 tile 之上）──停止/30 秒超时──▶ 回到响闹前那一页
```

- 不新增 tile（3-tile 结构不动），不改 `muse_ui.c` 的 tileview。
- 语言：设置页保持英文惯例（现有 SETTINGS/ALARM/Hour/Days 全是英文，`muse_settings_ui.c`）；响闹页用中文按钮“停止”（新开的提醒页，与今日运势/掷筊等互动页的中文一致）。标题 "ALARM" 保持英文（现有 `page(tile, "ALARM", …)`），时间设置页标题 "Set time"。
- 新文件 `muse_alarm_ui.{c,h}`：时间设置页（双滚轮，见 §3.1）与响闹覆盖层（见 §3.2）。时间设置页的导航（进页/返回保存）仍由 `muse_settings_ui.c` 接线：Alarm 子页的“时间”行点进滚轮页，返回时先保存再回 Alarm 子页——滚轮页的 LVGL 对象归 `muse_alarm_ui.c` 管，设置页的 `show()`/`drop()`/`go_back()` 机器不动（滚轮页的返回按钮与右滑手势都先保存再调设置页给的返回回调）。
- 响闹音的可换点（用户 2026-11-18 补充定版，见 §10-2）：响闹不用游戏的 `MUSE_SFX_WIN` 直出，而是音效表里单列的 `MUSE_SFX_ALARM`（终版音色：4000 Hz 双声蜂鸣，70ms/声、间隔 55ms、幅度 7000，`MUSE_ALARM_RING_MS` 250ms），循环间隔 `MUSE_ALARM_RING_MS` 与音符表集中在 `muse_audio.h` 的闹钟音色块里，注明“换音色只改这里”；换音色时只改那一块，不动 `muse_input.c` 的响闹逻辑。

## 5. 响闹行为（v1，分状态）

| 设备状态 | 触发 | 屏幕与声音 |
|---|---|---|
| 深睡 | RTC INT GPIO6 拉低 → ext1 唤醒 → boot → `alarm_start("boot")` | 亮屏 + 响闹页 + 4000 Hz 双声蜂鸣（每 250ms 一遍） |
| 轻睡 | GPIO6 结束轻睡 → `alarm_poll()` 读到 AF → `alarm_start("rtc")` | 同上 |
| 清醒 | `alarm_poll()` 每 500ms 读 AF 或时钟到达目标 | 同上 |
| 响闹中 | 停止按钮（不等宽限）/ BOOT / 触摸（6 秒宽限后）/ 30 秒超时 | 停响、撤页；once 关闹钟，其余重 arm 下一次 |
| 响闹中 | 小睡按钮 | **无（定版不要小睡）** |

- 声音：**4000 Hz 双声蜂鸣**（定版，见 §10-2）：响闹期间每 250ms 播一遍 `MUSE_SFX_ALARM`（两声 4000 Hz、每声 70ms、间隔 55ms、幅度 7000，每遍 195ms、遍间 55ms 静默，与声间间隔相同，连续约 8 声/秒），走主音量（Sound 页滑杆），无独立闹钟音量（v1）。音色块集中在 `muse_audio.h`（音符 + 循环间隔，注明“换音色只改这里”），换音色不动响闹逻辑。
- 响闹期间自动睡被抑制（现有 `muse_state_poke()`），响闹页不响应 tile 滑动。
- 30 秒超时自动停后：once 关闹钟，其余重 arm 下一次（现有 `alarm_stop("30 s")` 逻辑不变，只是时长由 60 秒改为 30 秒）。

## 6. 与唤醒源红线的关系（写清，不含糊）

- 1.4 定版“唤醒源只剩 BOOT”的原意：删了 **IMU GPIO8 与触摸 GPIO4** 两条唤醒线（板文件 `board_waveshare_s3_touch_lcd_185b.c:2530-2534` 注释、`:780-786`）。触摸唤醒已删，IMU 待机手势全部取消——这些 v1 **一字不动**。
- **RTC_INT GPIO6 已是第二唤醒源**，不是 v1 新加的，是 1.4a RTC 落地时就实现的代码事实：
  - 深睡：`power_off()` `:1786-1795`，`muse_rtc_wake_armed()` 为真时才把 `BIT64(MUSE_RTC_INT_GPIO)` 并入 ext1 掩码，否则日志明确写 "deep sleep: no alarm armed, BOOT only"；
  - 轻睡：启动时 `muse_gpio_wake_line_add_pullup(MUSE_RTC_INT_GPIO, false)`（`:754`，注释明说 armed 闹钟同样结束小睡）；
  - 头注释 `muse_rtc.h:55-64` 同样写明 RTC_INT "folds into the deep-sleep wake mask … while muse_rtc_wake_armed() is true, and it is a light-sleep GPIO wake source as well"。
- 关键限定：**只有用户把闹钟开关打开、且已 arm 的那段时间**，RTC_INT 才在唤醒掩码里；开关 off 时不留任何唤醒源，设备行为与 1.4 完全一致（BOOT only）。闹钟响完重 arm 下一次，掩码随之更新；once 闹钟响完关掉，掩码随之撤掉。
- **闹钟 ≠ 定时亮屏**（被否决的那个）：定时亮屏是“到点把屏点亮、本身无事”；闹钟是用户定的事件，声音是本体，亮屏只是它的通知面。v1 不加任何“到点亮屏”的独立功能，不加新的唤醒源。

## 7. 存储与断电

| 内容 | 存哪 | 断电/重启后 |
|---|---|---|
| alarm_h / alarm_m / alarm_on / alarm_days | settings NVS `"muse"`（现有） | 在（Flash 里的 NVS） |
| 小睡计数 / 小睡开关 | —（定版不要小睡，不新增 key、不进内存变量） | — |
| 下一次 arm 目标 | RTC 芯片闹钟寄存器 + `s_alarm_target` 内存 | 芯片侧在（寄存器掉电由纽扣电池维持）；内存侧丢，重启后 `input_task` 开头重 arm（现有 `:893-915` 逻辑） |

- 不进宠物 blob，不升 `MUSE_PET_VERSION`，不加迁移。settings NVS 是 key 制，新增 key 缺失时取默认值。
- 闹钟配置与 Wi-Fi/配对无关；恢复出厂（清 NVS）后回到默认 07:00 / 每天 / 开关 off。

## 8. 时间准确性

- 闹钟按本地时间比对：arm 时用 `localtime_r` + `mktime` 算目标（`alarm_arm_next()`），芯片存 UTC，显示时换算。时区走 `CONFIG_MUSE_TIMEZONE`，与顶栏时钟同一套。
- SNTP 跳变：v1 用 P5（同步后重 arm）修的是下一次 arm 的日期正确性，不是补响当日。往回调：F-A1 的已响日期标记（`s_alarm_last_rung_*`，`alarm_start()` 置位）让 `alarm_arm_next()` 跳过已响过的日期，不会立刻补响、也不会当天响第二遍；正向跳过目标仍丢当天那一响（见 §1.5）。残余边界：SNTP 回调与 arm 之间仍有竞态窗口（毫秒级），以及日期标记重启后丢失（§3.5）。写进验收清单实测。
- RTC 停振（Seconds.OS 位）：驱动掩掉该位，`muse_rtc_get()` 判无效；无有效时间时 `alarm_arm_next()` 打日志 "alarm: no time to count from yet" 并返回，不 arm（现有行为，v1 不动）。

## 9. 不做的项

- **多闹钟**：芯片只有 1 组闹钟寄存器；多闹钟要软件链（只 arm 最近一个，响完重 arm 下一个）+ 闹钟列表 UI，状态机与页面都翻倍。v2 再议。
- **独立闹钟音量**：v1 跟随系统音量（Sound 页滑杆）。pocket-watch 有 0–100 独立音量 + 试响，但它的喇叭路径与我们不同；单闹钟 + 全局音量已够用。
- **自定义星期掩码编辑**：4 档（Once / Every day / Workdays / Weekends）是 2026-10-23 用户定的，够用；"Custom" 只读显示。
- **闹钟标签/名字**：单个闹钟不需要名字。
- **语音设闹钟**（对毛毛说“明天 7 点叫我”）：要 `link.invoke` 新命令 + 服务端意图，不在固件 v1 里。
- **音乐/WAV 闹铃**：无音频文件机制；合成音够用（响闹音见 §5，`MUSE_SFX_ALARM`）。
- **手动设系统时间 UI**：与闹钟无关；时间只靠 SNTP + RTC adopt（pocket-watch 同款取舍）。
- **响闹时毛毛动画/宠物联动**：趣味项，v2。
- **闹钟与金币经济联动**（准时起床奖励金币之类）：不碰金币经济，v1 不做。

## 10. 待定清单（用户 2026-11-18 一句话一个点拍板，全部锁定）

1. **小睡要不要？**——**不要。** P3/P4 全删，不新增 `alarm_snooze` key，响闹页不画小睡按钮。
2. **响闹音**：**4000 Hz 双声蜂鸣**（用户 2026-11-18 选定 B 候选：DeadSounds “Electronic clock signal, alarm clock (beep, beep)”；实测原片约 8 声/秒、主频约 4kHz；转写为两声 4000 Hz、每声 70ms、间隔 55ms、幅度 7000，`MUSE_ALARM_RING_MS` 250ms，每遍 195ms、遍间 55ms 静默，与声间间隔相同，连续约 8 声/秒）。试听样例 `~/workspace/alarm-cand-b-beepbeep.wav`（原片 3 秒剪辑）与 `~/workspace/alarm-ring-b-beep.wav`（固件合成渲染 3 秒，以音符表为准）。WIN 琶音循环样例 `~/workspace/alarm-ring-win-loop.wav` 已废弃（用户觉得不够紧促）。
3. **时间设置**：**滚轮页**（§3.1），双滚轮时/分、离页一次性写 NVS + 重 arm。
4. **试响按钮**：**不要。**
5. **30 秒自动停**：由 60 秒改为 **30 秒**（`ALARM_RING_US`，`alarm_stop("30 s")`）。
6. **小睡时长**：同 1，**不要小睡**，无此时长。
7. **响闹页按钮**：**只留停止**；按键停止保持现有“任意键停”（BOOT 也是停止，不另加逻辑）。

## 11. 实现要点（给编码阶段，不含代码）

- 新文件 `muse_alarm_ui.{c,h}`：时间设置页（双滚轮，`muse_alarm_ui_build_time_page()` + 离页保存 `muse_alarm_ui_time_page_save()`）、响闹覆盖层（`muse_alarm_ui_tick()`，每帧读 `muse_input_alarm_ringing()`，在 `lv_layer_top()` 上显示/撤掉；input 任务不在 LVGL 线程，`alarm_start()`/`alarm_stop()` 不直接碰 LVGL）。LVGL 9.5，360×360 圆屏，布局按 `muse_board` 宽高相对算。
- Alarm 子页改动（`muse_settings_ui.c`）：Hour/Minute 两行合并为“时间”一行（值 `HH:MM`），点进滚轮页；`tick_alarm()` 跟着改；返回回调先保存再回 Alarm 子页。`muse_settings.{c,h}` 加 `muse_settings_set_alarm_time(h, m)`（两 key 一次保存、一次 `MUSE_SETTING_ALARM` 通知、一次重 arm）。
- 逻辑改动点：`muse_input.c`——`ALARM_RING_US` 60 秒→30 秒（`alarm_stop("30 s")`）；响闹音由 chirp 改为 `muse_audio_sfx(MUSE_SFX_ALARM)`，每 `MUSE_ALARM_RING_MS`（250ms）一遍（原 `ALARM_CHIRP_US` 2 秒节律与 `s_alarm_chirp_us` 随之改名 `ALARM_SFX_US`/`s_alarm_sfx_us`）；新增出口 `muse_input_alarm_stop()`（LVGL 停止按钮用，按钮不等 6 秒宽限；input 任务的按键路径与控制台 `>alarm.stop` 不动）；`muse_rtc.c` 的 `time_task` 同步回调——SNTP 同步、`muse_rtc_push()` 成功后调一次 `muse_input_alarm_sync()`（P5，响闹中该函数自行返回；重 arm 只保证下一次 arm 日期正确，往回调不重响、正向跳过仍丢当天，见 §1.5/§3.5）。
- 响闹音的可换点：`muse_audio.h` 新增 `MUSE_SFX_ALARM`（音效表里单列，终版音符：两声 4000 Hz、每声 70ms、间隔 55ms、幅度 7000）与 `MUSE_ALARM_RING_MS`（循环间隔，250ms），集中在“闹钟音色”块里并注明“换音色只改这里”；游戏的 WIN 不动。换音色时改的字段见该块注释。
- 删：`on_alarm_hour`/`on_alarm_min`（Hour/Minute 两行随 P1 退役）；`tap_direction()` 保留（Days 行仍在用）。
- 语言：设置页英文、响闹页中文按钮（§4）。用户可见文字说 Muse 不说 Hatch（仓库惯例）。
- Host 测试：新逻辑（滚轮页、响闹页、30 秒自动停、SNTP 重 arm）都在 ESP 侧——`muse_input.c` 主机侧编不过（与 1.5.3c 评审 B1 同款限制）、LVGL 页测不到、SNTP 回调在 FreeRTOS 任务里，进不了 harness；能进的是 `MUSE_SFX_ALARM` 的渲染（`muse_audio_sfx_render` 是纯函数，`tests/test_sfx.py` 的 harness 已覆盖 WIN 的 5280 采样，ALARM 同表同长）——全量跑过、与基线逐行 diff 零新增为准。写清哪些测不到，靠 §12 实机验收。
- 不动：`esp32/version.txt`、`ota/latest.json`、`.github/workflows/*`、tile 结构、金币经济、唤醒源。

## 12. 实机验收清单

- [ ] Alarm 子页 3 行 + 提示显示正常；开关开/关后主页行显示 `HH:MM` / `Off` 跟着变。
- [ ] 时间设置页：滚轮上下滑改时分，离开页面后 NVS 已保存（重启后还在）、下一次 arm 已更新（串口 `alarm %02d:%02d armed in %lds`）。
- [ ] 分钟能设到 05/07 这类 ±10 到不了的值。
- [ ] 滚轮页不误滑：滚轮上下拖时不触发 tile 横滑、不触发列表纵向滚动。
- [ ] 清醒响闹：到点亮屏 + 响闹页（大字时间 + 只有“停止”按钮，无小睡按钮）+ 4000 Hz 双声蜂鸣每 250ms 一遍；点“停止”停响撤页。
- [ ] 停止按钮不等宽限：响闹最初 6 秒内点“停止”立即停响（防幻触宽限只拦 BOOT/触摸）。
- [ ] BOOT 停响：6 秒宽限内按 BOOT 不停（防幻触），6 秒后按 BOOT 停。
- [ ] 轻睡响闹：灭屏后到点能响（屏亮 + 响闹页）。
- [ ] 深睡响闹：Power off（深睡）后到点能响（开机进响闹页）。
- [ ] 30 秒超时自动停；once 闹钟响完自动关（主页行变 `Off`）；每天闹钟响完重 arm 第二天。
- [ ] Days 四档：Workdays 只在周一至五响、Weekends 只在周六日响（可用 `>alarm.arm=N` 与改系统时间辅助验证）。
- [ ] SNTP 跳变：校时后重 arm（串口有重 arm 日志）；正向跳过目标仍丢当天（按 F-A1 定稿预期判），反向回调当天不响第二遍。
- [ ] 断电重启：闹钟配置还在，下一次 arm 恢复（串口 boot 行 `alarm_pending` 与 arm 日志）。
- [ ] 响闹期间按 BOOT 不进语音（talk 按压被吞），停后 2 秒内同一按压的释放不触发对话。
- [ ] 响闹音听感：4000 Hz 双声蜂鸣，每 250ms 一遍（连续约 8 声/秒），音量跟 Sound 页滑杆；换音色只改 `muse_audio.h` 的闹钟音色块。

## 附：效果图

`~/workspace/alarm-mockup-v1-{list,set,ring}.png`（S=3 超采样、360×360、圆形屏遮罩，Noto 16px + Montserrat 28/20，主题色同 `muse_theme.h`）：

- `alarm-mockup-v1-list.png`：Alarm 子页 v1（开关 / 时间 07:00 / 重复 每天 / 小睡 开 + 提示行）——**小睡行是旧版，定版只有 3 行**。
- `alarm-mockup-v1-set.png`：时间设置页（双滚轮 07 : 00 + 提示行）。
- `alarm-mockup-v1-ring.png`：响闹页（大字 07:00 + 停止 / 小睡 5分钟 + 提示行）——**小睡按钮是旧版，定版只留停止按钮（居中）**。

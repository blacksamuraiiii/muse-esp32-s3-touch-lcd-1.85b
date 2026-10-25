# 1.5a 第 1 阶段说明（模型 + 文本表 + worker + 串口方案）

> 状态：本地暂存，未编译、未合入仓库、未推 GitHub。
> 暂存根目录：`~/workspace/1.5-implementation/`，下设
> `esp32/components/muse/`（按仓库内相对路径放文件）。
> 第 1 阶段不含 UI tile、脸 UI、LVGL、游戏动画（第 2 阶段），
> 不碰 `muse_ui.c`、`muse_settings_ui.c`、avatar 文件、板级文件、main 分支。
> 代码内 `muse_pet_*` 原名保留（与 TamaMe 逐行比对用）；
> “UI 上不写宠物 / PET”只约束用户可见文案，代码名不在此限。

## 1. 文件清单

| 文件（暂存路径，相对 `esp32/components/muse/`） | 来源 / 说明 |
|---|---|
| `muse_pet_model.h` / `muse_pet_model.c` | 改编自 TamaMe `muse_pet_model.*`（Apache-2.0，文件头与署名保留）。金币经济专用版，见 §2。 |
| `muse_pet_text.h` / `muse_pet_text.c` | 自写，未 import TamaMe 的 `muse_i18n.h`。20 张运势卡 + 3 种食物名，见 §3。设计 §3.2 允许并入模型文件；本阶段保持独立文件，理由：文案与结算逻辑分离，用户审文案时不用翻模型代码。 |
| `muse_pet.h` / `muse_pet.c` | 改编自 TamaMe `muse_pet.*`（Apache-2.0，文件头与署名保留）。worker，见 §5。 |
| `muse_app_pet_patch.md` | `muse_app.c` 补丁说明（未应用）：加 `#include "muse_pet.h"`，在 `muse_time_start()` 紧后调 `muse_pet_start()`。 |
| `muse_input_pet_patch.md` | `muse_input.c` 补丁说明 + 完整代码片段（未应用）：`console_command()` 里加 `pet.*` 命令块。 |
| `CMakeLists_pet_patch.md` | `CMakeLists.txt` 改动说明（未应用）：共享核心段加 3 个 `.c` 文件。 |

未交付（有意不做）：`muse_interact_ui.*`、脸点按接入、字幕反馈接入、
`muse_pet_anim.*` / `muse_pet_art.*` 的 1.5a 版（食物图与手图是第 2 阶段
UI 资产，不属于第 1 阶段模型）。

## 2. 金币算式（设计 §4，逐条对应）

符号：`C` = 金币，`T` = 当日摸摸给币，`F` = 当日喂饭给币，
`D` = 自然日序号（`muse_pet_day()`，见 §6 待验证）。

- 钱包上限：任何进账后 `C = min(100, C + 进账)`，超出的部分直接丢掉。
  钱包截断时当日给币仍按全额计（例如钱包 98 时摸摸 +5，钱包到 100，
  `T` 仍 +5）：否则满钱包时可以靠截断绕过当日上限。
- 摸摸 `PET_TOUCH`：若当日 `T >= 50` → `PET_CAP`（只开心，不给币）。
  否则 `T += 5`，`C = min(100, C + 5)`，结果 `PET_COIN`。
  即前 10 次给币（`T` 到 50），第 11 次起 `PET_CAP`。
- 喂饭 `PET_FEED_RICE / PET_FEED_CAKE / PET_FEED_COFFEE`：三种结算完全相同。
  若当日 `F >= 10` → 仍返回 `PET_OK`，不给币（设计 §4.3：给币与否看快照
  的 `F`，不要另发明失败码；第 2 阶段 UI 据此写“今日奖励已领”）。
  否则 `F += 10`，`C = min(100, C + 10)`，结果 `PET_OK`。
  喂饭不花币：三种食物都没有价格，选哪个只是口味偏好。
- 小游戏 `PET_GAME`：若 `C < 5` → `PET_POOR`，不扣币、不开局。
  否则 `C -= 5`，结果 `PET_OK`。赢 / 平 / 输都不返币：胜负随机是第 2 阶段
  UI 侧的事，模型里没有胜负字段，也不存连胜（设计 §5.2：连胜只记本局显示）。
- 生活费（每天自动消耗）：设 `decay_day` 为上次结算日。
  - 时钟未校准（`now < 1735689600`，即 2025-01-01 UTC，取自 `muse_rtc.c`
    的“no real time yet”阈值）：不扣、不重置当日上限。
  - `decay_day == -1`（首次校准运行）：只把 `decay_day` 置为当日，不补扣。
  - 当日 `D > decay_day`：`due = 10 * (D - decay_day)`；
    `decay_notice = min(due, C)`；`C = max(0, C - due)`；`decay_day = D`。
    关机期间经过的天数也算（日历天数，不是运行天数），扣到 0 为止不欠账。
  - 当日 `D <= decay_day`（同日或时钟回拨）：不扣、不退款、不倒退 `decay_day`。
  - `decay_notice` 显示一次后清零：机制是内部动作 `PET_DECAY_ACK`
    （第 2 阶段 UI 在 IDLE 下一次互动显示完扣款额后发送；它不产生 feedback，
    所以串口命令里不要等它的 feedback）。已知局限：若在“显示”与“ack”
    之间恰好跨过午夜，tick 的新一轮扣款可能被这次 ack 一并清掉（见 §7 第 5 条）。
- 占卜 `PET_FORTUNE`：`fortune = random % 20`（`random` 来自 worker 的
  `esp_random()`）。
  - 时钟已校准且 `fortune_day == D` → `PET_FORTUNE_SAME`（显示同一张）。
  - 时钟已校准且 `fortune_day != D`（含首次 `-1`）→ 置 `fortune_day = D`，
    抽新卡，结果 `PET_FORTUNE_NEW`。
  - 时钟未校准：已有卡（`fortune_day >= 0`）→ `PET_FORTUNE_SAME`；
    从未抽过 → `PET_TIME`（不抽卡，因为“今天”无法定义）。
    设计未规定未校准占卜行为，此处为第 1 阶段的明确决策，见 §7 第 4 条。
  - 时钟回拨（`D < fortune_day`）：按 `SAME` 处理，不重抽、不倒退日期。

## 3. 文本表（设计 §4.5、§5.2）

- 运势卡：20 张，自写中文，非 TamaMe 文案、非六爻内容（六爻移植已于
  2026-10-24 搁置，卡片里不出现卦名、爻辞、六爻术语）。
- 等级：只用大吉 / 中吉 / 小吉 / 吉四档（4 / 6 / 5 / 5 张），无凶卡。
  这是为家庭陪伴场景做的取舍，不是上游传统分档；用户若要凶卡再加。
- 正文长度：目标 ≤ 32 字，最长不超过 2~3 行（360 圆屏深色卡片，18 号字）。
  文案仍是草稿措辞，需用户再审（见 §6）。
- 食物名：只含米饭 / 蛋糕 / 咖啡（`MUSE_PET_FOOD_RICE/CAKE/COFFEE`），
  火锅 / 鸡腿 / 茶已作废，不进表。
- 食物图（16×16 像素画）不在本阶段交付：第 2 阶段 UI 资产，来源是上游
  `muse_pet_art`，不是本阶段的文本表。

## 4. NVS 布局（设计 §4.4）

- Namespace：`pet_pub_stats`（沿用 TamaMe 名，diff 最小，便于逐行比对）。
- Key：`"state"`，一次写整个 `muse_pet_model_t` blob。
- 不塞进 `muse_settings` 的 `"muse"` namespace：`muse_settings_init()` 里
  `nvs_flash_init()` 在 `NO_FREE_PAGES` / `NEW_VERSION_FOUND` 时会擦整片
  NVS（1.4d `muse_settings.c` 第 112~116 行实测），宠物数据与系统设置同在
  NVS 分区里是既有行为，1.5a 不改；但宠物数据不抗 NVS 布局变化，
  Release 说明里不要承诺宠物数据永不丢。
- 写盘时机：只在状态变化时写（动作结算、跨日扣款、日上限重置、初始建表、
  初始跨日扣款），不每帧、不每 tick 写。先落盘再公布成功：
  写盘失败时结果置 `PET_STORAGE`，RAM 状态回滚到落盘前的状态，
  不公布未落盘的成功。
- 校验（`muse_pet_model_valid()`）：版本号 `== 2`、金币 0~100、
  当日摸摸给币 0~50、当日喂饭给币 0~10、占卜序号 < 20、
  四个日字段均 >= -1、待显示扣款额 <= 100。
- 损坏处理：blob 不存在 → 建初始状态并落盘；blob 损坏（读错、长度不符、
  校验失败）→ worker 记错（`muse_pet_storage_error() == true`）、
  奖励停用、worker 任务自删，**不静默重置**。恢复靠 Test 命令
  `>pet.reset`（见 §5 串口表）+ 重启重建；worker 不自动重试，
  避免在损坏 blob 上循环。
- 版本号说明：`MUSE_PET_VERSION` 定为 2（TamaMe 为 1）。结构布局与 TamaMe
  v1 不兼容，版本号不同 + 长度校验双重拦截，防止把 TamaMe 旧 blob 误读
  成 1.5a 状态。

## 5. Worker 参数与串口命令表

### Worker（设计 §6.2，TamaMe 形态）

- 独立 FreeRTOS 任务 `"muse_pet"`，命令队列 8、反馈队列 8、
  30 秒一轮（`xQueueReceive` 超时即检查一次日切分，不做高频轮询）、
  优先级 2、栈 4096。
- 优先级复核（实测）：1.4d `muse_input.c` 里 input 任务优先级 6、
  serial 任务 5；pet worker 优先级 2 低于两者。voice 任务优先级本次未抽到，
  见 §6 未决项。
- 栈位置：内部 RAM。`xTaskCreate` 默认即内部 RAM，不要改成 PSRAM 栈：
  NVS 提交会关 cache，PSRAM 栈不安全（TamaMe 注释原话，设计 §6.2）。
  代码注释里已写明这一条。
- 功耗：30 秒一轮、无每小时衰减、无常驻扫描、不持有 power lock、
  不阻止浅睡；动作与跨日扣款才写 NVS。1.5a 不新增宠物音效。
- 反馈队列假设（TamaMe 原样保留的风险）：worker 用 `portMAX_DELAY`
  发 feedback，队列满时 worker 会停住；因此消费方（第 2 阶段 UI tick，
  或第 1 阶段串口等待循环）必须每轮排空 feedback 队列，否则 worker 停滞。
  这是 TamaMe 的原设计，不是本阶段新引入的假设，见 §7 第 6 条。
- 启动：`muse_pet_start()` 幂等（`s_started` 守卫），失败只记日志；
  插入点见 `muse_app_pet_patch.md`（`muse_time_start()` 紧后，UI / 语音之前）。
  已删 TamaMe 的自起 SNTP（本项目 `muse_time_start()` 已做 RTC adopt + SNTP）。

### 串口命令（设计 §8.3，`muse_input.c` 补丁，尚未应用）

| 命令（串口敲 `>...`） | 门控 | 行为 |
|---|---|---|
| `>pet.status` | 常驻 | 只读：打印金币、当日摸摸/喂饭给币、占卜日与序号、扣生活费日与待显示扣款额、worker 就绪态。无快照时只打印就绪态与存储错误标志，不编造数字。 |
| `>pet.touch` | 验证用，不包 Test 宏 | 发 `PET_TOUCH`，等 feedback（最长 ~2 秒），打印结果码 + 快照。 |
| `>pet.feed` | 验证用，不包 Test 宏 | 发 `PET_FEED_RICE`（串口默认米饭；三种喂饭结算相同，验的是结算不是食物），等 feedback，打印结果码 + 快照。 |
| `>pet.fortune` | 验证用，不包 Test 宏 | 发 `PET_FORTUNE`，等 feedback，打印结果码 + 快照 + 当日卡的等级与正文。 |
| `>pet.game` | 验证用，不包 Test 宏 | 发 `PET_GAME`，等 feedback，打印结果码 + 快照。只验扣 5 币；胜负随机是第 2 阶段 UI 侧的，模型里没有。 |
| `>pet.reset` | **只在 Test 构建内**（`#if CONFIG_MUSE_BUILD_TEST`） | 破坏性。worker 运行时走队列 `PET_RESET`（无 race）；worker 已死（如 blob 损坏）时直接擦除 blob，重启后重建。Release 下该行落入“unknown command”，不是静默无事。 |

注意：`pet.touch` / `pet.feed` / `pet.game` 会真实改金币（加币 / 扣币），
不是无副作用的“只读”——设计 §8.3 的“只读验证用”指“不走 UI 的验证路”，
不是指不改状态。既有 bench 命令（`power.reset`、`alarm.test`）同样不包
Test 宏且改状态，这是既有惯例，不是本补丁新开的口子。

## 6. 未决项（必须在合入 / 发布前闭环，不要带进 Release 说明）

1. **时钟语义（待验证，最高优先级）**：`muse_time_start()` 之后 `time()` 是
   UTC 还是本地时区，决定占卜日与生活费日的公式。当前代码用
   `localtime_r()` + 民用日期换算（`days_from_civil`）算本地日历日，
   前提是 `time()` 为 UTC epoch 且 TZ 已由 `muse_time_start()` 设置。
   实测线索（非定稿结论）：`muse_rtc.c` 里 `muse_time_start()` 做
   `setenv("TZ", CONFIG_MUSE_TIMEZONE)` + `tzset()` 再 `muse_rtc_adopt()`，
   且 RTC 文件头写明 “UTC in and out”、adopt 日志按 UTC 打印——这指向
   “time() 为 UTC epoch、localtime 为本地时区”，与当前实现一致，
   但按设计 §6.1 与任务要求，仍标为待验证，不凭此定稿。
   验证方法：串口 `>time=YYYY-MM-DD HH:MM:SS`（UTC）设一个已知日期，
   观察 `>pet.status` 的占卜日 / 生活费日是否按上海本地日切分；
   另查 `CONFIG_MUSE_TIMEZONE` 的实际取值（本次未抽到 Kconfig 默认值）。
   若实测证明 `time()` 已是本地时区，`muse_pet_day()` 会二次偏移，
   必须重写日公式（例如改用 UTC 日 + 固定偏移，或改用 `mktime` 路径），
   不得只改注释蒙混。
2. **栈与 voice 优先级复核**：栈 4096、优先级 2 是 TamaMe 起点；
   已验证低于 input（6）与 serial（5），但 voice 任务优先级本次未抽到，
   合入前必须确认 worker 优先级不高于 voice 任务，且 4096 栈在实机上
   不溢出（NVS 提交路径的栈深以实测为准）。
3. **占卜卡文案需用户再审**：20 张卡为本阶段自写草稿，等级无凶卡是
   为家庭陪伴场景做的取舍；最终措辞、分档（是否加凶卡）由用户定。
4. **未校准时钟下的占卜行为**：设计未规定。本阶段决策：已有卡则显示旧卡，
   从未抽过则返回 `PET_TIME`、不抽卡（见 §2）。合入前由主 agent 转用户确认。
5. **内部动作的已知局限**：`PET_DECAY_ACK` 不产生 feedback（串口不要等它）；
   若在 UI“显示扣款”与“发送 ack”之间恰好跨过午夜，tick 的新一轮扣款
   可能被这次 ack 一并清掉（窗口极小，第 2 阶段 UI 应在同一 tick 内显示并
   ack）。若要彻底消除，需给扣款加序号，1.5a 不做。
6. **反馈队列满则 worker 停滞**：TamaMe 原设计（`portMAX_DELAY` 发 feedback）。
   第 2 阶段 UI tick 必须每轮排空 feedback 队列；第 1 阶段串口等待循环里
   不匹配的 feedback 会推回队首（`muse_pet_feedback_requeue()`），
   队满推回失败时该条 feedback 丢失并打印警告。
7. **`esp_random.h` 可用性**：首次编译时验证；若报缺头文件，再补
   `esp_system` 到 `PRIV_REQUIRES`，不要预先加（1.4d 的 CMakeLists 未列它，
   且 ESP-IDF 核心组件通常已可达）。

## 7. 设计稿与 1.4d 代码的冲突 / 出入（原样记录，不擅自改设计稿）

1. **`muse_build.h` 没有 pet.reset 的 Release 校验项**：设计 §3.3 说
   “进 Release 前必须已被编译期排除，靠构建验证”，但实测 `muse_build.h`
   只对 `CONFIG_MUSE_POWER_TEST` / `CONFIG_MUSE_TELEMETRY` 做 Release
   `#error`，没有针对 pet.reset 的校验。本阶段只做到
   `#if CONFIG_MUSE_BUILD_TEST` 宏门控，不擅自改 `muse_build.h`；
   合入时决定是否补校验项，或接受宏门控。
2. **设计 §3.2 的互动 UI 行仍写返回 `>`**：本地修订已定左侧 `<`，
   §3.2 表格里 `muse_interact_ui` 一行的“右上角 `>` 返回按钮”已过期。
   第 1 阶段不碰 UI，记给第 2 阶段，不算第 1 阶段阻塞项。
3. **设计 §3.2 的“加 4 个新文件”与实际不拢**：`srcs` 实际新增 3 个 `.c`
   文件，连同 `.h` 共 6 个文件。见 `CMakeLists_pet_patch.md`，以 3 个 `.c` 为准。
4. **设计 §2.5 的 worker 插入点已核实无冲突**：`muse_app.c` 实测顺序为
   `muse_time_start()` →（Test）`muse_telemetry_start()` → `muse_ui_start()`
   → `muse_input_start()` → `muse_voice_start()`，本阶段插入点
   （`muse_time_start()` 紧后）满足“UI 与语音尚未开始”。
5. **设计 §8.3 的 `>pet.game`“随机结果”**：第 1 阶段串口只验扣 5 币，
   不输出胜负，因为胜负随机按设计 §5.2 是第 2 阶段 UI 在点按时定的，
   模型里不应再引入一份随机。若主 agent 认为串口也应输出胜负，
   需先定“模型随机还是 UI 随机”的唯一口径，不要两份随机并存。
6. **TamaMe worker 用的 `muse_mem.h`**：1.4d 是否有该文件本次未核实，
   本阶段 worker 不需要它（无堆分配），故不引入该依赖，不算冲突。
7. **设计 §4.1 的“保留不用字段恒定不动”与本阶段的删除**：本阶段直接删掉
   饥饿 / 心情 / 能量 / sfx / last_time 字段（比“保留但不动”更强，
   它们不可能再影响金币），并把版本号升到 2 以拦截旧 blob。
   若主 agent 要求保留字段以便与 TamaMe 逐行 diff，可在合入时加回
   恒为 0 的占位字段，但本阶段不推荐。

## 8. 第 1 阶段验收自查（对照设计 §8、§10 第 1 步）

- [ ] 首次编译通过（Test 构建链；`esp_random.h`、栈、优先级见 §6）。
- [ ] 串口全流程（不接 UI）：`>pet.status` → `>pet.touch`（前 10 次 +5，
  第 11 次 `CAP`）→ `>pet.feed`（第一次 +10，第二次仍 `OK` 但不加币）
  → `>pet.fortune`（同日两次同一张）→ `>pet.game`（<5 币时 `POOR` 不扣，
  ≥5 币时扣 5）→ 重启后 `>pet.status` 数值都在。
- [ ] 跨日：改系统日期（`>time=`）后 `>pet.status` 的当日给币清零、
  生活费按天数扣、扣款额只在下一次互动显示一次（第 2 阶段 UI 侧，
  第 1 阶段以快照 `decay_notice` 为准）。
- [ ] 损坏 blob：写坏 NVS blob 后 worker 记错、奖励停用、不静默重置；
  `>pet.reset`（Test）+ 重启后重建。
- [ ] 时钟未校准：`now < 2025-01-01` 时不扣生活费、不重置当日上限、
  占卜无卡时返回 `TIME`。
- [ ] 时钟回拨：不补发金币、不补扣生活费、不倒退各日字段。
- [ ] 本阶段不产生以下回归（因未动相关文件）：对话、闹钟、顶栏、待机、
  IMU / 触摸唤醒（均未动，worker 不阻止浅睡）。

# 打砖块移植设计（2026-11-18）

> 状态：设计定版，代码未写。分支 `feat/breakout-20261118`，基点 main `803ca78`
> （1.5.3 正式版）。效果图：v4 定版布局（`~/workspace/breakout-mockup-v4*.png`、
> `~/workspace/breakout-demo-v3.gif`）；开始页 v5 只改提示行
> （`~/workspace/breakout-mockup-v5-start.png`，脚本
> `~/workspace/breakout-mockup-v5.py`）。
> 本机无 ESP-IDF 工具链：本文档只定设计，不含编译结论；编译与实机验收由用户做。

## 0. 依据：核对过的代码（不是凭记忆写的）

2026-11-18 在工作克隆里逐行核对：

| 文件 | 核对到的事实（本文档照抄的点） |
|---|---|
| `esp32/components/muse/muse_goldrain_ui.c`（772 行） | 覆盖层规矩、入场费门（snapshot + `PET_FEE`）、`settle_round`（OVER 时一次换算）、倾斜（每 tick 一次 `muse_imu_read_accel`、低通 0.35、死区 0.5、增益 40、符号 −1、滑行上限）、拖动备用、长按暂停（手记 500ms、漂移 ≤12px）、`muse_state_poke`、HUD/拒绝共用 y=308、开始页不压暗、暂停/结算 50% 压暗、中央按钮 98×66 (131,147) CARD 底 + 2px ACCENT 边 |
| `esp32/components/muse/muse_doodle_ui.c`（822 行） | 同上另一份印证；另有 `muse_imu_wake()` 在 show / begin / resume 三处都调、`muse_float_text` 共用浮字 |
| `esp32/components/muse/muse_goldrain.{c,h}` | 逻辑层无 LVGL、无 ESP-IDF，时间只经 `tick(ms)` 推进；`TICK_MS 50`；`set_catcher` 只在 PLAYING 生效；host 测试形态见下 §10 |
| `esp32/components/muse/muse_interact_ui.c` | 注册接线见 §8（行号以基点 `803ca78` 为准） |
| `esp32/components/muse/muse_pet_model.h:57,112` | `MUSE_PET_GAME_STAKE 10u`、`MUSE_PET_ROUND_COIN_MAX 20u`；换算只走 `muse_pet_round_coins()` |
| `esp32/components/muse/muse_audio.h:78-90` | v4 音效表（GOOD/BAD/DRAW/PLACE/WIN/START 等，全部现場合成，无音频文件） |
| `esp32/components/muse/muse_imu.h:46-56` | `muse_imu_available()` / `muse_imu_wake()` / `muse_imu_sleep()` / `muse_imu_read_accel()` |

**两处旧注释已过期，照抄会抄错，特此钉住：**

- `muse_goldrain_ui.h` 头注释仍写“每局 5 金币”“上限 10 金币”——与现行宏（10 / 20）和 `.c`
  实现不符。移植照抄 `.c` 的做法、取宏值，不抄 `.h` 的旧注释。
- `muse_goldrain.{c,h}` 头注释也残留“上限 10 金币”的旧数；同样以
  `MUSE_PET_ROUND_COIN_MAX`（20，2026-11-08 改）为准。
- v4 结算效果图（`breakout-mockup-v4-over.png`）的 HUD 写“本局 12分，+2金币”：
  12 分按 gross 口径应付 +12，+2 是净得（12−10）的数，与“只写 gross、不写净得”
  的规矩冲突，属效果图数字笔误。实现以 gross 为准（§6）；v5 不动结算图，只在
  §6 留档，免得后人照着 +2 实现。

## 1. 决策锁表

| 项 | 锁定值 | 出处 |
|---|---|---|
| 球初速 | 约 **120 px/s**，常量 `MUSE_BREAKOUT_BALL_SPEED` | §3 |
| 球 vx 上限 | **220 px/s**（模拟的办法） | §3 |
| 挡板最高速 | **260 px/s**，常量 `MUSE_BREAKOUT_PADDLE_MAX_PX_S` | §4（含推导） |
| 生命 | **每局 3 条** | §3 |
| 砖 | 6 列 × 4 行 = 24 块，每块 +1 分 | §2、§3 |
| 结算 | 打光 24 块或 3 条命用完 → OVER；付 `min(分, 20)` | §3、§6 |
| 入场费 / 兑付上限 | 10 / 20（`MUSE_PET_GAME_STAKE` / `MUSE_PET_ROUND_COIN_MAX`） | §6 |
| 主控 | QMI8658 倾斜；手指拖动备用；长按中间暂停（500ms、≤12px） | §4 |
| 音效 | 只复用 v4 现有音，不新增 | §7 |
| 提示行 | “左右倾斜移动挡板”（y=304 DIM；2026-11-18 用户定去掉“，长按中间暂停”） | §5 |
| 计时器 | 无（跟贪吃蛇一样不带倒计时） | §5 |

## 2. 三件套架构

照抄金币雨，不自创第三种结构：

```
esp32/components/muse/muse_breakout.{c,h}      逻辑层：无 LVGL，无 ESP-IDF
esp32/components/muse/muse_breakout_ui.{c,h}   UI 层：自建 lv_timer，50 ms 一 tick
esp32/components/muse/muse_interact_ui.c       注册进小游戏列表（§8 清单）
esp32/components/muse/CMakeLists.txt           加 muse_breakout.c、muse_breakout_ui.c 两行
esp32/tests/test_breakout_logic.py             host 测试（§10）
esp32/tests/breakout_logic_harness.c           host 测试桩（照抄 goldrain_logic_harness.c）
```

- **无 art 文件。**砖、挡板、球全部 LVGL 手画（圆角矩形/圆），沿用打地鼠炸弹
  （`bomb_obj`）、毛毛跳跃云朵（`puff`/`cloud_obj`）的“容器 + 子件”画法，
  不新增图片资源、不新增 flash 块。
- 逻辑层 API 照抄金币雨的命名（`init / start / pause / resume / restart /
  tick / set_paddle`），另加 `lives`、`bricks_left` 两个只读量：
  - `muse_breakout_init(b)` / `muse_breakout_start(b)` /
    `muse_breakout_pause(b)` / `muse_breakout_resume(b)` /
    `muse_breakout_restart(b)`——语义与金币雨一一对应（start 只从 START 进，
    restart 任意态进新局，resume 不重复收费）。
  - `muse_breakout_tick(b, ms)`——只在 PLAYING 推进；暂停时 UI 停 timer，
    tick 不被调到（与金币雨相同）。
  - `muse_breakout_set_paddle(b, cx)`——只在 PLAYING 生效，夹到场内。
  - 随机数只用 `rand()`，由 UI 在首次开始时 `srand(lv_tick_get())`（金币雨
    `seed_once` 的做法）；逻辑层自己不取时间、不调 `lv_tick_get()`。
- UI 层静态状态照抄金币雨的形状：`s_game`、`s_settled`、`s_round_coins`、
  `s_ax_filt`、`s_tilt_cx`、按压跟踪（`s_press_active / s_press_moved /
  s_press_start_ms / s_press_x / s_press_y`）、`s_timer`、六个页面对象
  （`s_touch / s_start_ov / s_paused_ov / s_over_ov / s_back / s_name /
  s_hud / s_note`）。

## 3. 布局与几何（与 v4 效果图一致）

场：240×240，位于 (60,60)，即 x∈[60,300]、y∈[60,300]。场本身只有描边
（圆角 6、2px `BORDER` 线，效果图 `draw_field` 的做法；不像毛毛跳跃那样铺
CARD 底），场内黑底。

| 元素 | 几何 | 颜色/画法 |
|---|---|---|
| 砖 | 6 列 × 4 行；36×16，列间距 4、行间距 6；第 c 列 x = 62 + c×40，第 r 行 y = 70 + r×22；圆角 3 | 行色自上而下红 / 黄 / 绿 / 紫（`ROW_COLORS`）；每块带一条 4px 高的提亮顶条（亮 60 的同色，效果图做法；固件里是砖容器的子件） |
| 挡板 | 72×10，圆角 5；y = 286（场底上 14px，即 `FY+240-14`），中心 x 活动范围 96…264 | 紫色 `ACCENT` 底 + 3px 浅紫高光顶条（效果图做法） |
| 球 | 白点 r=5（10×10 圆，半径 5） | 白色 |
| 碎砖“+1” | 碎砖的空格里居中 | 金色 16px，共用 `muse_float_text()`（金币雨/毛毛跳跃同款，上浮淡出） |

砖区占 y=70…70+4×16+3×6=152；挡板顶 y=286；球的活动纵深约 70…300。

### 3.1 球速：初速 120 px/s

模拟（`breakout-demo-v3.gif`）用的约 160 px/s 太快，定 **120 px/s**，常量
`MUSE_BREAKOUT_BALL_SPEED`（`muse_breakout.h`，可调）：

- 240 宽的场，120 px/s 横穿约 2 秒；纵向从挡板（y≈281）到顶行砖（y≈70）
  约 211 px，vy 分量 69…98 px/s 时一程约 2.2…3.1 秒——7 岁孩子跟得上，
  且有时间把挡板移到位。
- 发球：球从场中 (180, 260) 出发，仰角 35°…55°（相对水平，向上），vx
  正负随机，速度 120 px/s（即 vx ≈ 69…98、vy ≈ −69…−98）。沿用模拟的
  发球公式，只把 160 换成 120。
- 速度不随时间递增（v1 不做难度爬坡，见 §12）。vx 只能由挡板反弹改，
  见 3.2。

### 3.2 碰撞（按模拟的办法）

- 每 tick（50ms）球走 `v × 0.05`：初速时每步 6px；vx 到上限 220 时横向
  每步 11px。**无隧道**：11px < 砖厚 16px + 球径 10px = 26px，单 tick
  不可能穿过一整块砖，逐 tick 的 AABB 重叠检测足够，不用扫掠检测。
- 墙壁：左/右（球心 x < 65 或 > 295）翻 vx；顶（球心 y < 65）翻 vy；
  静音（太频繁，见 §7）。
- 砖：每 tick 最多碎一块（先扫到先算，按行自上而下）。用**上一 tick
  的球心**定反弹轴：上一球心在砖的上/下方 → 翻 vy；在砖的左/右方 →
  翻 vx；角落歧义时翻 vy（砖是横排，竖直反弹是常见情形）。反弹后把球
  沿反弹轴推出重叠区（贴砖边 ±(r+0.5)），免得下 tick 重复撞同一块。
  碎砖：该格熄灭、+1 分、`bricks_left--`、砖心浮“+1”（金色）、GOOD
  音（§7）。24 块打光 → 立即 OVER（不等球落下），走结算。
- 挡板：只在 vy>0 且本 tick 球底越过挡板顶线（y=286）时接住，
  且 |球心 x − 挡板中心| ≤ 41（半个挡板 36 + 球半径 5，模拟同值）。
  命中偏移 `off = (球心x − 挡板中心) / 36`，夹到 [−1, 1]；
  `vx = clamp(vx + off × 90, −220, +220)`；`vy = −|vy|`；球 y 置为
  286 − 5，防粘住。打中挡板中央 → vx 几乎不变、直上直下；打中边缘
  → 大角度斜飞。**vx 上限 220 px/s**（模拟同值，见 §4 推导的用途）。
- 无残影、无运动线：球是单个 LVGL 对象，每 tick 只改位置，不留拖影；
  不画速度线（毛毛跳跃“身后不加速度线”是同一条规矩）。

### 3.3 生命：每局 3 条

- 球心 y > 300（掉出场底）即掉一命：BAD 音；`lives--`。
- `lives` 剩 >0：球与挡板一起复位（球回 (180, 260)、挡板回中心 180），
  按 3.1 重新发球（新随机角），本局继续，不扣分、不重新收费。
  v1 不设发球等待态（发球立即开始；若实机觉得太赶，再加等待态，见 §12）。
- `lives` 到 0 → OVER，走结算（§6）。结算条件只有这两条：3 命用完，
  或 24 砖打光。

## 4. 挡板控制：倾斜主控 + 拖动备用

### 4.1 倾斜：沿用金币雨整套标定

- QMI8658 轮询 `muse_imu_read_accel()`，**约 50Hz**（每 50ms tick 读
  一次，与 `on_tick` 同频）；只在打砖块页打开时轮询，离页即停
  （`muse_imu_wake()` / `muse_imu_sleep()`，§9）。
- 标定数照抄金币雨（1.5.1 实机闭环）：符号
  `MUSE_BREAKOUT_TILT_SIGN = -1.0f`、增益 40 px/(m/s²)、死区 0.5 m/s²、
  平滑 0.35。目标位置 = 场中心 + SIGN × 增益 × 滤波后 ax；死区内挡板
  不动（端平不回中）。读数失败当 tick 跳过（沿用上次目标）。
- 滑行不瞬移：每 tick 向目标最多走 `上限 × 0.05`（§4.2 的上限），
  目标由拖动写入时同样适用（`s_tilt_cx` 共享，金币雨的做法：拖动后
  倾斜不把挡板拽回去）。

### 4.2 挡板最高速：260 px/s（推导）

金币雨的 130 px/s 上限**不够**，必须重算。约束是挡板必须跑赢小球的
横向速度，否则球往一侧底角去时接不住：

1. 最坏情形是 vx 取上限 220 px/s（§3.2 挡板边缘反弹可以把 vx 推到
   220）。球横穿 240 宽的场需 240 / 220 ≈ **1.09 秒**。
2. 挡板中心活动范围 96…264，行程 168 px。
3. 金币雨的 130 px/s 走完 168 px 需 168 / 130 ≈ **1.29 秒 > 1.09 秒**——
   球从一侧墙弹向另一侧底角时，挡板追不上。130 出局（不是“有点慢”，
   是算出来的不够）。
4. 取 **260 px/s**：走完 168 px 需 168 / 260 ≈ **0.65 秒**，比球横穿
   快 0.44 秒，留给孩子反应与倾斜建立；裕度 260 / 220 ≈ 1.18。
   每 tick 最多 13 px，不跳变。
5. 这个数是按**最坏 vx=220** 定的，不是按初速 120 定的：初速时球横穿
   要 2 秒，260 的裕度更大。日后若改球速或 vx 上限，260 必须跟着重算。
6. 倾斜增益侧能喂饱 260：35° 倾斜约 5.6 m/s² → 目标偏移
   40 × 5.6 = 224 px，覆盖 168 px 全行程，挡板不会因目标不够远而
   跑不满速。

常量 `MUSE_BREAKOUT_PADDLE_MAX_PX_S = 260.0f`（UI 层，与
`MUSE_GOLDRAIN_TILT_MAX_PX_S` 同位置、同注释体例）。

### 4.3 手指拖动（备用）与长按暂停

- 照抄金币雨的触摸层：全页透明 `s_touch`，只在 PLAYING 显示。
  PRESSED 把挡板直接放到手指下（同时写 `s_tilt_cx`）；PRESSING
  1:1 跟手（拖动不受 260 上限约束，手指放哪挡板在哪）。
- 长按中间暂停：手记 500ms、漂移 ≤12px（`MUSE_BREAKOUT_HOLD_MS 500u` /
  `MUSE_BREAKOUT_HOLD_MOVE_PX 12`，全仓统一做法；不用 LVGL 的
  LONG_PRESSED，免得拖动中途误暂停）。判定放在 `on_tick` 开头、
  `update_tilt` 之前（金币雨的顺序）；移动超 12px 即记为拖动，
  永不触发暂停。
- 无暂停按钮（全仓 2026-11-01 的决定：34px 按钮在圆屏月牙区会被切）。

## 5. 覆盖层统一规矩（照抄金币雨 / 毛毛跳跃，不自创）

页面级（在压暗层之上，永远可读）：标题位 y=38、ACCENT、16px
（`muse_page_title(page, "打砖块", 38)`）；HUD y=308、居中、16px。

| 状态 | 标题 y=38 | HUD y=308 | 覆盖层 | 中央按钮 98×66 (131,147) CARD 底 + 2px ACCENT 边、圆角 14 |
|---|---|---|---|---|
| 开始 | 打砖块 | 隐藏（提示行占位） | **不压暗**（场可见） | 只有居中“开始” |
| 游玩 | 打砖块 | 白字计分-only“本局 n 分” | 无 | 无（返回箭头隐藏） |
| 暂停 | 已暂停 | 白字“本局 n 分” | 全屏 360×360、50% 压暗 | 只有 ▶符号 + “继续”（符号 +12、“继续” +34） |
| 结算 | 游戏结束 | 金字“本局 n 分，+m 金币”（只写实得 gross，不写净得） | 全屏 360×360、50% 压暗 | 只有“再来一局” + 金字“- 10 金币”（标签 +12、价钱 +34） |

- 卡片上不写标题、不写结算、无“返回”按钮（离开走左上角返回箭头；
  游玩时隐藏返回箭头，先长按暂停才能离开——贪吃蛇的规矩）。
- 开始页提示行 y=304、DIM、16px，文案锁定：
  **“左右倾斜移动挡板”**（v6，见下；2026-11-18 用户定去掉“，长按中间暂停”半句）。
- 开始页 HUD 隐藏：提示行与 HUD 同槽（y=304/308），两者不同时出现
  （金币雨 `sync_message_band` 的做法照抄）。
- 拒绝提示（金币不够）与 HUD 共用 y=308 槽，短文案
  “金币不够（需 10 枚）”（由 `MUSE_PET_GAME_STAKE` 格式化，不写死数字）。

### 5.1 开始页 v6 与提示行宽度

v4 开始页提示行“左右滑动挡板，长按中间暂停”（13 字、208px）已过时——
主控改成倾斜了。v5 改为“左右倾斜移动挡板，长按中间暂停”（15 字、
240px），首尾两字的底角贴圆边，留作实机核对项 A10。2026-11-18 用户定
去掉“，长按中间暂停”半句，v6 只剩“左右倾斜移动挡板”（8 字、128px），
远在 y=304 行顶部弦宽 261px、底部（y=320）弦宽 226px 之内，**A10 的
贴边风险随之消除**，实机只需确认可读。

## 6. 经济（小游戏统一模型）

- 入场费 10（`MUSE_PET_GAME_STAKE`），开始 / 再来一局时先 snapshot
  门 + `PET_FEE`（照抄金币雨 `charge_for_game`：余额不足 → 拒绝提示
  + BAD 音、不扣费；worker 复核失败 → `muse_breakout_ui_fee_refused()`，
  局作废回 START，见 §8）。
- 每块砖 +1 分；满分 24 分。结算只在 OVER 做一次：
  `muse_pet_round_coins(score)` → 付 `min(分, 20)`（`MUSE_PET_ROUND_COIN_MAX`），
  经 `muse_pet_request_play_reward()` 一次入账；`s_round_coins` 只记钱包
  实际收到的数（被拒即 0，HUD 诚实写 +0）。
- 满分局：24 分 → 付 20，净 +10（20−10），保本线仍是 10 分，与其他
  游戏一致。HUD 只显示 gross（“本局 n 分，+m 金币”），不写净得
  （用户定：孩子分不清净得）。
- 暂停后继续不重复收费；离开页面只暂停、不结算（§9）。
- 分不设负值（没有炸弹/扣分项），不需要 `settle_score` 式的 floor。

## 7. 音效：只复用 v4 现有音，不新增

| 事件 | 音 | 理由 |
|---|---|---|
| 砖碎（每块） | `MUSE_SFX_GOOD`（880→1318Hz，120ms） | 得分音，与金币雨接金币 / 毛毛跳跃吃金币同一只 |
| 挡板接球 | `MUSE_SFX_PLACE`（900Hz，45ms） | 最短的一只；接球最频繁，用短促的“落子”声，不与得分音混淆 |
| 球掉出底边（扣命） | `MUSE_SFX_BAD`（300→150Hz，120ms） | 失误音，与金币雨接炸弹同一只 |
| 开始 / 再来一局 | `MUSE_SFX_START`（上升 chirp） | 与其他游戏 `begin_game` 同一只 |
| 结算 | `MUSE_SFX_WIN`（523→659→784 琶音，330ms），入账 0 时 `MUSE_SFX_DRAW`（660Hz） | 按实际入账数选音，与金币雨/毛毛跳跃的结算规则相同 |
| 金币不够被拒 | `MUSE_SFX_BAD` | 与其他游戏的 `charge_for_game` 同一只 |
| 墙壁 / 砖边反弹 | 静音 | 太频繁；毛毛跳跃的碎云也是静音 |

音量与喇叭开关走现有 `muse_audio_sfx()`（跟音量滑杆、喇叭开关），
不另设游戏音量。

## 8. 注册进小游戏列表（照抄金币雨的接线方式）

基点 `803ca78` 的 `muse_interact_ui.c` 里，金币雨的接线点就是打砖块
的接线点，逐项照抄（行号以基点为准，动手前复核）：

1. 枚举加 `IC_PAGE_BREAKOUT`（金币雨 `IC_PAGE_GOLDRAIN` 约 228 行处）。
2. 页面指针 `*s_breakout_page` 与名字标签 `*s_mg_breakout_lbl`
   （约 248–256 行，`s_mg_goldrain_lbl` 旁边）。
3. `show_page()`：hide 清单加一行、show 分支加一行（约 387–408 行）。
4. `on_back()`：离页时调 `muse_breakout_ui_hide()`；回落分支进
   `show_page(IC_PAGE_MINIGAME)` 那组（约 479–510 行）。
5. `on_open_breakout()`：`show_page(IC_PAGE_BREAKOUT)` +
   `muse_breakout_ui_show()`（`on_open_goldrain` 约 1391 行处同款）。
6. `build_minigame()`：strip 末尾、OX棋之后加一张卡
   `minigame_card(strip, MUSE_MG_ICON_BREAKOUT, "打砖块",
   on_open_breakout, &s_mg_breakout_lbl)`（约 1899–1903 行）。
   顺序：追加在末尾，不动现有六张卡的顺序（用户 2026-11-08 定的顺序）。
   图标新增 `MUSE_MG_ICON_BREAKOUT`：LVGL 手画迷你场（场框 + 两行迷你
   砖 + 挡板 + 球点，80px 槽），沿用 `mg_icon_snake` / `mg_icon_board`
   的手画 idiom，不新增图片资源；画法细节随实现出图给用户看一眼。
7. `refresh_status()`：`s_mg_breakout_lbl` 加 dimming 行（余额 <10 时
   变 DIM，与其他五张收费卡同款；约 586–612 行）。注：dimming 不是
   disable——点进去照样由局内门拒绝并提示。
8. `PET_FEE` 反馈的两个分支（`PET_POOR` 与 `PET_STORAGE`，约 1258–
   1280 行）：各加 `else if (s_page == IC_PAGE_BREAKOUT)
   muse_breakout_ui_fee_refused();`，路由按可见页，别碰到别的游戏。
9. `muse_interact_ui_build()`：`s_breakout_page = page_group(tile)` +
   `muse_breakout_ui_build(s_breakout_page, on_back)`（约 2259–2281 行）。
10. `muse_interact_ui_on_hidden()`：加 `muse_breakout_ui_hide();`
    （约 2312–2325 行）。离 tile 时互动页重置回主页面（全仓规矩）。
11. 注释里的“六个游戏”要改成七个（`muse_interact_ui_swipe_locked`
    注释、卡片顺序注释等几处，别留着旧数误导后人）。
12. `CMakeLists.txt`：`muse_breakout.c`、`muse_breakout_ui.c` 加进
    `muse_goldrain.c` / `muse_doodle.c` 旁边（约 57–64 行）。

UI 层另需提供的三个钩子（与金币雨/毛毛跳跃同名同义）：
`muse_breakout_ui_build(page, back_cb)` /
`muse_breakout_ui_show()` / `muse_breakout_ui_hide()` /
`muse_breakout_ui_fee_refused()`。

## 9. 电源与待机纪律（不新增唤醒源）

- 倾斜轮询只在打砖块页打开时跑：`muse_imu_wake()` 在 `show` /
  `begin_game` / `on_resume` 三处调（毛毛跳跃的做法）；`hide` 时
  `muse_imu_sleep()`。离页即停，不动待机功耗。
- 不新增任何唤醒源（1.4 定版：IMU 手势与触摸唤醒已删；打砖块的轮询
  只在亮屏游玩时跑，与 1.4 取消的待机手势不是一回事——掷筊摇动同款
  论证）。
- 每 tick `muse_state_poke()`（纯倾斜游玩不碰屏，不 poke 会睡屏，
  金币雨同款）。
- 离页 / 藏 tile 时正在跑的局只暂停、不结算、不丢进度（回访从暂停
  覆盖层继续）；重启回 START（进度只放 RAM，与其他游戏一致，不写 NVS）。
- BOOT 保持全局行为（回对话页），不是游戏键。

## 10. Host 测试（照抄金币雨的形态）

逻辑层无 LVGL、无 ESP-IDF，host 测试用 `cc -std=c11 -O2 -Wall -Wextra
-Werror -fsanitize=address,undefined` 编 `muse_breakout.c` + 桩
（`tests/test_breakout_logic.py` + `tests/breakout_logic_harness.c`，
照抄 `test_goldrain_logic.py` 的结构）。桩里必覆盖的情形：

1. 发球：速度 |v| = 120，向上，vx 符号随机（多跑几次两种符号都出现）。
2. 墙壁：左/右/顶反弹，vy/vx 符号翻转，位置夹回场内。
3. 砖：正上方撞砖碎一块、+1 分、翻 vy；侧面撞砖翻 vx；一 tick 最多碎一块。
4. 挡板：中央命中 vx 几乎不变；边缘命中 vx 变大（方向与偏移同号）；
   vx 夹到 ±220；vy 翻正；球被推出到挡板顶上。
5. 掉命：球出底边 lives 3→2，球与挡板复位，分数保留；3 次掉完进 OVER。
6. 打光：24 块全碎进 OVER（不等球落下）。
7. 状态机：`start` 只从 START 进；`pause/resume` 只在 PLAYING/PAUSED
   之间；`set_paddle` 在非 PLAYING 是 no-op；`tick` 在非 PLAYING 是 no-op；
   挡板夹到 96…264。
8. 结算边界：分 24 → `muse_pet_round_coins` 付 20（桩里断言换算只取
   min(分, 20)，不由逻辑层自己算钱）。
9. 挡板追得上：vx=220 的球横穿 1.09 秒内，挡板以 260 px/s 能走完
   168 px（0.65 秒）——以 tick 步进算，不以一次跳变算。

## 11. 实机验收清单（用户侧，编译后跑）

- A1：进小游戏列表看到第七张卡“打砖块”，点进是开始页（场 + 24 块砖
  + 挡板 + 开始钮 + v5 提示行），返回箭头回列表。
- A2：金币 <10 点开始 → 拒绝提示 + BAD 音，不扣费、不进局。
- A3：开始扣 10；球速体感：横穿约 2 秒，不觉得赶；7 岁孩子能跟上。
- A4：倾斜左右移挡板方向对（反了就翻 `MUSE_BREAKOUT_TILT_SIGN`，
  金币雨 1.5.1 同款修正）；端平挡板不动；轻晃不漂移。
- A5：挡板追得上底角球：故意把球打向一侧底角，260 上限下接得住
  （§4.2 的推导就验这一条）；拖动备用同样接得住。
- A6：砖碎一块 +1、浮“+1”、GOOD 音；打光 24 块进结算，HUD 写
  “本局 24分，+20金币”（不是 +24、不是净得 +10）。
- A7：掉一命 BAD 音、复位重发；3 命掉完进结算，HUD gross 正确。
- A8：长按中间 500ms 暂停（标题变“已暂停”，只有“继续”钮）；
  拖动中途不误暂停；暂停时返回箭头可离开，回访从暂停继续。
- A9：球无拖影、无运动线；墙壁反弹静音；挡板接球是短促的 PLACE 音。
- A10：提示行实机核对：15 字整行可读、首尾字底角无可见切边；
  若切边，按 §5.1 备选缩为 13 字。
- A11：离页 / 藏 tile / BOOT 回对话页后，局暂停不结算、不丢分；
  重进从暂停继续；重启回 START。

## 12. 只评估、没动手的东西

- **难度爬坡**（球速随打掉砖数/时间提升）：评估过，未定进 v1。
  若以后加，vx 上限 220 与挡板 260 必须一起重算（§4.2 的推导以 220
  为输入），不能只改球速。
- **发球等待态**（掉命后球停 1 秒再发）：评估过，v1 不做（立即重发）；
  若实机觉得太赶再加，不改经济与结算。
- **特殊砖**（多血砖、加宽挡板、加命道具）：评估过，v1 不做；24 块
  全是普通砖。
- **列表卡图标**：方案已定（§8 第 6 项，LVGL 手画迷你场），但画法
  细节须随实现出图给用户看一眼再锁。
- **音效映射**（§7）：映射已定，但 PLACE 作接球音、GOOD 作碎砖音
  是纸面选型，须实机试听确认不混淆、不吵。
- **v5 提示行贴边**（§5.1）：出图自查通过，实机仍须按 A10 核对。
- **v4 结算图数字笔误**（§0）：只留档，未重出结算图（v5 只改开始页）。
- 代码、CMakeLists、host 测试、编译、实机验收：全部未动手，等用户
  点头再开工。

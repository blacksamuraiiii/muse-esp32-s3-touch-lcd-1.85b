# muse_ui.c 补丁说明（第 2 阶段，合入时手工应用）

> 本文件不是补丁文件本身：远端仓库文件不由第 2 阶段直接改动，
> 合入时按下面 7 个点逐项改 `esp32/components/muse/muse_ui.c`
>（基线：分支 `docs/1.5-interaction-preview` 上的 1.4d 代码，
> 行号为合入前该分支的 `muse_ui.c` 行号，仅作锚点，改动时以
> 锚点文字为准，不要按行号硬套）。
> 所有改动只在 `CONFIG_MUSE_BUILD_TEST` 下生效的 1.5a 分支上做，
> 不要动 main / 1.4d。改完后必须编译通过再谈实机。

## 0. 前置：include

在 `muse_ui.c` 顶部加：

```c
#include "muse_interact_ui.h"
#include "muse_pet.h"
```

（`muse_pet.h` 只为 `PET_TOUCH`；`muse_state.h` 已有。）

## 1. 静态变量：加互动 tile，圆点改 3 个

锚点：`static lv_obj_t *s_settings;` / `static lv_obj_t *s_dots[2];`
（约 121–122 行）。

```c
static lv_obj_t *s_interact;
static lv_obj_t *s_settings;
static lv_obj_t *s_dots[3];
```

## 2. 建 tile：互动 (0,0)、脸 (1,0)、设置 (2,0) —— 方向常量照抄

锚点：`muse_ui_start()` 里 `build_screen()` 的 tileview 段
（约 925–938 行），原文：

```c
s_face = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
...
s_settings = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);
```

改成：

```c
/* Order is interaction, face, settings (design §2). Direction constants
 * follow the 1.4d pattern verbatim: a tile's direction is where you can
 * GO FROM it, so the face (middle) gets LEFT|RIGHT, not a guessed
 * "left swipe / right swipe" mapping. */
s_interact = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
lv_obj_set_scrollbar_mode(s_interact, LV_SCROLLBAR_MODE_OFF);
lv_obj_set_style_bg_color(s_interact, lv_color_black(), 0);
lv_obj_set_style_bg_opa(s_interact, LV_OPA_COVER, 0);
s_face = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
lv_obj_set_scrollbar_mode(s_face, LV_SCROLLBAR_MODE_OFF);
lv_obj_set_style_bg_color(s_face, lv_color_black(), 0);
lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
s_settings = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_LEFT);
```

注意：

- `face = s_face;` 那行不用改（脸的内容仍建在 `s_face` 上，
  只是坐标从 (0,0) 变 (1,0)）。
- `muse_ui_show_face()` 不用改：它是指针式
  （`lv_tileview_set_tile(s_tv, s_face, ...)`），坐标变化自动跟随。
- `tile_changed_cb()` 不用改：它比较指针 `== s_face`，不是坐标。

## 3. 建互动 tile 内容 + 默认首页仍是脸（关键，容易漏）

锚点：`muse_ui_start()` 末尾，`muse_settings_ui_build(s_settings);`
之后、`build_overlays();` 之前（约 1839 行）：

```c
if (s_interact) {
    muse_interact_ui_build(s_interact);
}
```

再在 `build_overlays();` 之后、`lv_timer_create(frame_tick, ...)` 之前加：

```c
/* Tileview defaults to its FIRST tile, which is now the interaction tile.
 * Without this line the device would boot onto the interaction tile.
 * The default home is still the face (design §2). */
if (s_tv) {
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
}
```

## 4. 底部圆点：2 个 → 3 个，偏移 -16/0/+16

锚点：`build_overlays()` 里 `for (int i = 0; i < 2 && s_tv; i++)`
（约 1253 行）：

```c
for (int i = 0; i < 3 && s_tv; i++) {
    ...
    lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - 1) * 16, -14);
    s_dots[i] = d;
}
```

（原来是 `i ? 8 : -8`；三点时中间点偏移 0，两边 ±16。）

## 5. 设置页 tick 段：页索引改三值，圆点隐藏只认设置子页

锚点：约 1415–1430 行的设置 tick 段。原文是二值
（`page = active == s_settings`，`shown = page * 2 + subpage`，
圆点循环 `i < 2`，隐藏条件 `page && subpage`）。

改成：

```c
if (s_tv) {
    lv_obj_t *active = lv_tileview_get_tile_active(s_tv);
    int page = active == s_interact ? 0 : (active == s_face ? 1 : 2);
    bool subpage = muse_settings_ui_in_subpage();
    bool swipe = !(page == 2 && subpage);
    /* NOTE: the interaction tile does NOT use set_swipe_enabled-style
     * subpage locking (design §5.2): tile swipe stays enabled inside
     * interaction subpages, so `swipe` here only ever locks for
     * settings subpages. Do not extend this to page 0. */
    if (swipe != lv_obj_has_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, swipe);
    }
    int shown = page * 3 + (subpage ? 1 : 0);
    if (shown != s_shown_page) {
        for (int i = 0; i < 3; i++) {
            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == page ? COLOR_ACCENT : COLOR_DOT_OFF), 0);
            /* Dots hide ONLY on a settings subpage. An interaction
             * subpage is not a top-level tile change, so the dots stay. */
            lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, page == 2 && subpage);
        }
        s_shown_page = shown;
    }
    /* ⚠ 下面这行是 1.4d 写法，不能照搬：原来脸在 (0,0)、设置在 (1,0)，
     * 所以 scroll_x > 0 等价于“在设置页”。三 tile 后脸在 (1,0)，
     * scroll_x > 0 在脸页也成立，会让设置 UI 在脸页也被 tick。
     * 必须改成指针比较： */
    muse_settings_ui_tick(active == s_settings);
}
```

## 6. frame_tick：互动 tick 放最前 + 离开互动 tile 的清理 + 滑动判断改脸偏移

锚点：`frame_tick()`（约 1736 行）。分三处：

### 6a. 互动 tick 放 frame_tick 最前面（在任何 early return 之前）

```c
static void frame_tick(lv_timer_t *timer)
{
    /* Interaction tick runs every frame, BEFORE any early return below:
     * it drains the pet feedback queue (a full queue stalls the worker,
     * phase 1), expires the pet caption, and advances the game animation.
     * It must not be gated on the face being visible or on the scroll
     * position -- feedback can arrive while the user is on another tile,
     * and the decay notice must be able to announce on the next IDLE
     * frame even if the interaction tile was never opened. */
    if (s_tv) {
        lv_obj_t *active = lv_tileview_get_tile_active(s_tv);
        static bool was_interact = false;
        bool is_interact = (active == s_interact);
        if (was_interact && !is_interact) {
            muse_interact_ui_on_hidden();
        }
        was_interact = is_interact;
        muse_interact_ui_tick(is_interact);
    }
    ...（原有 frame_tick 内容）...
}
```

说明：

- `muse_interact_ui_tick()` 本身不阻塞、不持 power lock、不做
  高频轮询（它只在 LVGL 帧 tick 里跑，见
  `muse_interact_ui.h` 的线程契约）。
- 字幕过期清理**不要**在 `muse_ui.c` 里另写一份：
  互动 tick 里的 `caption_tick()` 已负责（只清仍是互动字幕的那条，
  且只在 IDLE 时清）。两份清理逻辑必打架。
- `was_interact` 用函数内 static：离开互动 tile 时清 note、
  关子页、取消游戏动画（见 `muse_interact_ui_on_hidden()`）。
  注意滑动过程中 `lv_tileview_get_tile_active()` 可能提前报
  目标 tile：这只会让 note 早清一帧，不会误清脸页内容。

### 6b. 滑动静止判断：`scroll_x != 0` 改成“脸未完全可见”

锚点：约 1774 行：

```c
if (s_tv && lv_obj_get_scroll_x(s_tv) != 0) {
    /* Off screen, or sliding to or from settings: hold still ... */
    return;
}
```

⚠ **三 tile 后不能照搬**：原来脸在 (0,0)，`scroll_x != 0` 等价于
“脸不在屏上”。现在脸在 (1,0)，其完全可见时的 `scroll_x` 是
一个 tile 宽（= 屏幕宽 `s_w`），照搬会让脸页永远 early return、
脸动画冻住。改成：

```c
if (s_tv && lv_obj_get_scroll_x(s_tv) != s_w) {
    /* Face not fully on screen (off screen, or mid-slide): hold still
     * so the slide gets the whole frame time. Face is the middle tile,
     * so its fully-visible scroll offset is one screen width. */
    return;
}
```

合入时实测验证：滑动动画中 `scroll_x` 是否确实在两 tile 偏移之间
连续变化、且停稳后精确等于 `s_w`（若 LVGL 的 tile 宽与 `s_w`
有 1px 级出入，改用“与 s_w 差值 < 2” 的容差判断，不要硬等）。

### 6c. 其余 frame_tick 段不用改

- `update_control_hints()` 里 `settings = active == s_settings` 是
  指针比较，三 tile 下仍然正确。
- `s_bench_page` 段用指针选 `s_settings` / `s_face`，仍然正确。
- `muse_ui_show_face()` 指针式，仍然正确。

## 7. 脸点按：on_canvas_clicked() 加 PET_TOUCH 双通道

锚点：约 547–551 行，原文只有 `muse_state_make_happy();`。
改成：

```c
static void on_canvas_clicked(lv_event_t *e)
{
    (void)e;
    /* Sync channel, unchanged: the face goes happy immediately. */
    muse_state_make_happy();
    /* Async channel (design §5.1): coins go through the pet worker's
     * feedback queue. Guards, in order -- all must hold, else only the
     * happy channel runs and no coins move:
     *   1. IDLE (a tap during a dialog is not a touch reward);
     *   2. face tile visible (muse_ui_face_active() -- the canvas only
     *      exists on the face tile, but a stale event must not pay);
     *   3. point not on a face control (speaker / aux icon);
     *   4. not dark (screen asleep: taps wake, they do not pay).
     * The request never blocks: on failure (worker not ready / queue
     * full) only the flag is set, and the interact tick turns it into
     * an "互动未就绪" caption on the next IDLE frame. */
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        return;
    }
    if (!muse_ui_face_active()) {
        return;
    }
    if (muse_ui_dark()) {
        return;
    }
    if (s_indev) {
        lv_point_t pt;
        lv_indev_get_point(s_indev, &pt);
        if (muse_ui_face_control_at(pt.x, pt.y)) {
            return;
        }
    }
    if (!muse_pet_request(PET_TOUCH)) {
        muse_interact_ui_touch_failed();
    }
}
```

注意：

- 摸摸**不在**互动页里放按钮（设计 §5.2）：脸点按是唯一入口。
- 不要在这里直接写字幕或调 `muse_state_set_caption`：字幕的
  唯一写路径是互动 tick 里的 feedback 消费（设计 §5.3）。
- `muse_ui_face_control_at()` 已有（约 535 行，speaker / aux
  图标各扩 12px 判定），直接复用，不要重写点判逻辑。
- `s_indev` 在 `muse_ui_start()` 里由 `display_start()` 填好；
  若为 NULL（不该发生在触摸板上），跳过第 3 步而不是拒绝摸摸：
  canvas 本身被点按已说明点不在别的控件上。

## 不改的东西（合入 review 时逐项核对）

- `muse_menu.c`：不动。1.85B 的 `aux_button = NULL`，互动入口
  不是那套双键菜单。
- `muse_settings_ui.c` 的卡片/按钮构件是 static：互动 tile
  没有复用它们，而是按同样式常量另写构件（见
  `muse_interact_ui.c` 文件头）。不要为了“复用”把 settings
  的 static 改成导出 —— 那会动 1.4d 的设置页。
- `muse_ui_set_swipe_enabled()`：语义不变；互动子页不调它。
- IMU / 触摸唤醒 / GPIO4 / power lock：本阶段不引入任何一项。

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tracing_ui.h -- 描红写字 (语文练习) subpages inside the
 * interaction tile (user decision 2026-11-20, mockup v1).
 *
 * Two pages, two levels: the 学习 page's "语文练习" row opens the
 * character list (muse_tracing_ui_build_list: a 2x2 grid of
 * 104x104 cards, 10 characters over 3 swiped pages 4/4/2, a dot per
 * page at y=340; each card shows its character in the 48 px tracing
 * face with its pinyin underneath, and a green tick when that
 * character already paid today), and a card opens that character's
 * trace page (muse_tracing_ui_build_trace: a 228x228 田字格 centred
 * at (66,66) -- red solid border, dashed red cross -- with the
 * character's ghost inside (the 48 px face scaled to 170 px, faint
 * fill) and the child's ink drawn over it in white ~10 px lines).
 * Back goes one level up at a time: trace page -> list -> 学习 page.
 *
 * No start page (the word-cards "进组即玩" precedent: tapping a
 * character opens its trace page directly), no summary page (a
 * finished trace answers on the spot with "写得真棒！" in gold and
 * three gold stars, and a ">" at the right edge opens the next
 * character), no long-press pause (there is no clock; leaving the
 * page stops the attempt). A wrong trace flashes the ink red with
 * "描错了，再试一次" for 900 ms, then clears the ink and retries
 * (user decision 2026-11-20). Grading is stroke-shape only, no
 * stroke-order check: muse_tracing.c owns the scoring, this module
 * only renders and samples touch.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / OX棋
 * subpages: the page groups live inside the interaction tile (NOT a
 * top-level tile), and this module owns only what is inside that
 * group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM trace state (muse_tracing.c);
 * the reward is settled by the pet worker through
 * muse_pet_request_tracing_solve(), exactly once per finished trace,
 * and the snapshot is the authority for the coin line -- this module
 * never touches the wallet itself. The trace page shows the praise
 * whether or not the character already paid today; only the payout
 * is once a day (user decision 2026-11-20), so the praise never
 * prints "+2 金币".
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_tracing_ui_hide) stops the timer and drops the current pen
 * stroke, so a trace never finishes invisibly; a reboot restarts
 * from the list. The module never touches voice: screen taps are
 * consumed by the touch layer, and BOOT keeps its global behaviour
 * (it is not a game key), exactly like the other mini-game pages.
 *
 * Tuning knobs (real-board pass): MUSE_TRACING_UI_MIN_MOVE_PX (ink
 * sampling), the MUSE_TRACING_* thresholds in muse_tracing.h, and
 * the ghost's faint fill (MUSE_TRACING_UI_GHOST_OPA). The grid's
 * (66,66) offset is shared with the logic's field coordinates
 * (MUSE_TRACING_FIELD): touch points are converted by subtracting
 * it, so a layout move must move both.
 */
#pragma once

#include "lvgl.h"

/* Callback the list page calls when a character card is tapped: the
 * owner (muse_interact_ui.c) shows the trace page for that
 * character. `idx` is 0..9, the MUSE_TRACING_TABLE index. */
typedef void (*muse_tracing_open_char_cb_t)(int idx);

/* Build the character list inside `page` (a 360x360 group created by
 * muse_interact_ui.c). `back_cb` is the back-arrow handler (back to
 * the 学习 page); `open_cb` runs when a character card is tapped.
 * Safe to call once, in the LVGL task. */
void muse_tracing_ui_build_list(lv_obj_t *page, lv_event_cb_t back_cb,
                                muse_tracing_open_char_cb_t open_cb);

/* Build the trace page inside `page` (a 360x360 group created by
 * muse_interact_ui.c). `back_cb` is the back-arrow handler (back to
 * the list page). The page's content (which character, its ghost,
 * its ink) is set by muse_tracing_ui_open_char(); until then it
 * shows the first character. Safe to call once, in the LVGL task. */
void muse_tracing_ui_build_trace(lv_obj_t *page, lv_event_cb_t back_cb);

/* Prepare the trace page for character `idx` (0..9): fresh attempt,
 * ghost + progress rebuilt, ink cleared, praise hidden. The owner
 * calls this before showing the trace page. No-op for an
 * out-of-range index (the page keeps its current character). */
void muse_tracing_ui_open_char(int idx);

/* Refresh the list page's green ticks from the snapshot (today's
 * paid mask): the owner calls this when showing the list page, so a
 * character paid on the trace page ticks on the way back. No-op
 * before the list page is built. Safe in the LVGL task. */
void muse_tracing_ui_refresh_list(void);

/* Stop the timer and drop the current pen stroke (the trace page
 * keeps its character and its finished ink, like the sudoku page
 * keeps its puzzle). Called by muse_interact_ui_on_hidden(). */
void muse_tracing_ui_hide(void);

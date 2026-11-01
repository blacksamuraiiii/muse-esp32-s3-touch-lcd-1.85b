/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_sudoku_ui.h -- 6x6 Sudoku subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune subpages:
 * the page group lives inside the interaction tile (NOT a top-level
 * tile), and this module owns only what is inside that group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM puzzle state (muse_sudoku.c);
 * there is no worker and no NVS here -- coin movement is two commands to
 * the pet worker, PET_SUDOKU_ENTRY and PET_SUDOKU_SOLVE (see below).
 *
 * Economy (user decision 2026-11-01, superseding the per-puzzle fee of
 * 2026-10-31): entering sudoku costs 5 coins ONCE PER CALENDAR DAY, and that
 * one ticket covers everything else the page does that day -- re-entering,
 * 上一题, 下一题 and 再玩一次 are all free. The request is idempotent in the
 * worker, so the page simply asks again on each action instead of keeping a
 * local "paid" flag that a reboot would lose. Completing a puzzle pays 5
 * coins, capped at 5 per day: only the day's first solved board pays, and
 * later ones say "今日数独金币已满". A day's sudoku can therefore at best
 * refund its own entry fee.
 *
 * Progress lives in static RAM: leaving the tile (muse_sudoku_ui_hide) closes
 * the pad popup and the win overlay but keeps the puzzle, so re-entering the
 * page resumes it (no charge once the day's ticket is paid); a reboot
 * restarts puzzle #01 and the day's ticket state survives in NVS.
 */
#pragma once

#include "lvgl.h"

/* Build the Sudoku page inside `page` (a 360x360 group created by
 * muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: Sudoku back always lands on the main
 * interaction page). Safe to call once, in the LVGL task. */
void muse_sudoku_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: closes any popup / win overlay left open and
 * refreshes the grid from the kept puzzle state. Progress is NOT
 * reset. */
void muse_sudoku_ui_show(void);

/* Leaving the tile: closes the pad popup and the win overlay. The
 * puzzle state is kept, so a return visit resumes the puzzle. */
void muse_sudoku_ui_hide(void);

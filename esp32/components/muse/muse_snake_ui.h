/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_snake_ui.h -- Snake subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / sudoku
 * subpages: the page group lives inside the interaction tile (NOT a
 * top-level tile), and this module owns only what is inside that group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state (muse_snake.c);
 * there is no worker, no NVS and no coin settlement here -- the entry
 * fee goes through the pet worker as PET_FEE (see below), the score
 * here is food x 10 and is never currency.
 *
 * Economy (user decision 2026-10-31): each game costs 5 coins.
 * Tapping 开始 on the START overlay or 重开 on the OVER overlay (or
 * on the pause overlay) first checks the pet snapshot: under 5 coins
 * the start is refused with "金币不够，先摸摸 / 喂饭 / 抽运势" and
 * nothing is charged; otherwise PET_FEE is queued and the game
 * starts. Resume after pause never charges again, and a restart
 * counts as a new game and charges again. The worker is the only
 * writer of coins; a stale snapshot that lets a fee through is
 * settled by the worker (PET_POOR feedback is ignored here -- the
 * snapshot is the authority and no second game can deduct
 * concurrently, so the gate cannot be beaten in practice).
 *
 * Progress lives in static RAM: leaving the tile (muse_snake_ui_hide)
 * pauses a running game, so a return visit resumes it from the pause
 * overlay; a reboot restarts from START. The game never touches
 * voice: screen taps are consumed by the touch layer, and BOOT keeps
 * its global behaviour (it is not a game key), exactly like the
 * other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Build the Snake page inside `page` (a 360x360 group created by
 * muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: Snake back always lands on the main
 * interaction page). Safe to call once, in the LVGL task. */
void muse_snake_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: refreshes the board from the kept game state.
 * Progress is NOT reset: a paused game stays paused, a finished game
 * stays on its OVER overlay, an unstarted game stays on START. */
void muse_snake_ui_show(void);

/* Leaving the tile: pauses a running game (timer stopped, pause
 * overlay shown), so it cannot play on invisibly. Any other state is
 * left as-is, so a return visit resumes exactly where it left off. */
void muse_snake_ui_hide(void);

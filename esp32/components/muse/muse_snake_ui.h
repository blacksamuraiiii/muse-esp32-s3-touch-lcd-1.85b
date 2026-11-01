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
 * there is no worker and no NVS here -- coin movement is two commands to
 * the pet worker, PET_FEE at start and PET_PLAY_REWARD once at OVER
 * (see below).
 *
 * Economy (user decision 2026-10-31, payout 2026-11-01): each game costs
 * 5 coins. Tapping 开始 on the START overlay or 再玩一次 on the OVER
 * overlay (which prints "- 5 金币" under the label) first checks the pet
 * snapshot: under 5 coins the start is refused with "金币不够…" and
 * nothing is charged; otherwise PET_FEE is queued and the game starts.
 * Resume after pause never charges again, and a restart counts as a new
 * game and charges again. The worker is the only writer of coins; a
 * stale snapshot that lets a fee through is settled by the worker
 * (PET_POOR feedback is ignored here -- the snapshot is the authority
 * and no second game can deduct concurrently, so the gate cannot be
 * beaten in practice).
 *
 * The score (1 point per food) is converted exactly once, when the round
 * ends, by muse_pet_round_coins() -- 1 point per coin (corrected
 * 2026-11-01 from 5, which paid a 13-point round only 2 coins), at most
 * 10 coins per round (2026-11-01, was 5) -- and sent as PET_PLAY_REWARD.
 * The payout is gross: the 5-coin entry fee was already charged at start,
 * so a round nets min(score,10) - 5 (10 points = +5, 5 points = 0, 0
 * points = -5, the same win/draw/loss as rock-paper-scissors) and no round
 * mints more than 5 coins. The result is shown once, under the board: the
 * line is white and score-only ("本局 n 分") while PLAYING or PAUSED, and
 * turns GOLD at OVER with the gross payout ("本局 n 分，+m 金币", UI polish
 * 2026-11-01). The net is deliberately NOT printed -- the user said kids
 * cannot tell net apart -- and the OVER card carries no line of its own
 * (the page name becomes "游戏结束"). Leaving the page mid-round pauses
 * and settles nothing.
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

/* PET_FEE came back refused (PET_POOR / PET_STORAGE) for a round this
 * page optimistically started: reset to START -- the round never
 * settled, so no payout may fire from it -- and show the short
 * refusal note. Same contract as muse_whackamole_ui_fee_refused.
 * Call from the LVGL task only (interact's drain_feedback). */
void muse_snake_ui_fee_refused(void);

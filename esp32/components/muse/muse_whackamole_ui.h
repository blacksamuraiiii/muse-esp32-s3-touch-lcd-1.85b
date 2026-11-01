/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_whackamole_ui.h -- whack-a-mole subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / sudoku
 * subpages: the page group lives inside the interaction tile (NOT a
 * top-level tile), and this module owns only what is inside that group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state (muse_whackamole.c);
 * there is no worker and no NVS here -- coin movement is two commands to
 * the pet worker, PET_FEE at start and PET_PLAY_REWARD once at OVER
 * (see below).
 *
 * Economy (user decision 2026-10-31, payout 2026-11-01):
 * each game costs 5 coins. Tapping 开始 on the START overlay or
 * 再玩一次 on the OVER overlay (which prints "- 5 金币" under the
 * label) first checks the pet snapshot: under 5 coins the start is
 * refused with "金币不够（需 5 枚）" -- the short copy, because the note
 * now shares the slot under the field, where only ~213 px of chord is
 * left (2026-11-01) -- and nothing is charged; otherwise PET_FEE is
 * queued and the game starts. Resume
 * after pause never charges again, and there is no restart button on
 * the pause card (2026-11-01, same decision as snake: one tap in the
 * bezel must not spend 5 coins). The worker is the only writer of
 * coins; if its re-check finds the snapshot was stale (PET_FEE
 * feedback PET_POOR), the interact tile calls
 * muse_whackamole_ui_fee_refused(), which cancels a running game back
 * to START with the refusal note.
 *
 * Score and payout: one mole is one point, and the 30 s round's score is
 * converted exactly once, at OVER, by muse_pet_round_coins() -- 1 point per
 * coin (corrected 2026-11-01 from 5, which paid a 13-point snake round only
 * 2 coins), at most 10 coins per round (2026-11-01, was 5) -- then sent as
 * PET_PLAY_REWARD. The payout is gross: the 5-coin entry fee was already
 * charged at start, so the round nets min(score,10) - 5 and no round mints
 * more than 5 coins. The result is printed once, under the field: white and
 * score-only plus the 30 s countdown ("本局 n 分 · 剩余 X 秒") while PLAYING
 * or PAUSED, GOLD with the gross payout ("本局 n 分，+m 金币") at OVER (UI
 * polish 2026-11-01, same slot as the snake page). The net is deliberately
 * NOT printed (user: kids cannot tell net apart) and the OVER card carries
 * no line of its own (the page title becomes "游戏结束"). Leaving the page
 * mid-round pauses and settles nothing.
 *
 * Pause is a LONG PRESS on the field (user request 2026-11-01, "参考贪吃
 * 蛇"): the old 34 px pause button sat outside the round bezel, so only a
 * clipped sliver of it could be hit. A press that hit a mole never pauses,
 * and the back arrow stays hidden while playing -- long-press first, then
 * leave from the pause card.
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_whackamole_ui_hide) pauses a running game, so a return visit
 * resumes it from the pause overlay; a reboot restarts from START.
 * The game never touches voice: screen taps are consumed by the touch
 * layer, and BOOT keeps its global behaviour (it is not a game key),
 * exactly like the other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Build the whack-a-mole page inside `page` (a 360x360 group created
 * by muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: whack-a-mole back always lands on the
 * mini-game menu). Safe to call once, in the LVGL task. */
void muse_whackamole_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: refreshes the board from the kept game state.
 * Progress is NOT reset: a paused game stays paused, a finished game
 * stays on its OVER overlay, an unstarted game stays on START. */
void muse_whackamole_ui_show(void);

/* Leaving the tile: pauses a running game (timer stopped, pause
 * overlay shown), so it cannot play on invisibly. Any other state is
 * left as-is, so a return visit resumes exactly where it left off. */
void muse_whackamole_ui_hide(void);

/* PET_FEE feedback came back PET_POOR (the snapshot gate was stale
 * and the fee never settled): cancel a running game back to START
 * with the refusal note. Called from the interact tile's feedback
 * drain, on the LVGL task. */
void muse_whackamole_ui_fee_refused(void);

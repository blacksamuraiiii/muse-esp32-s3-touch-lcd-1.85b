/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_breakout_ui.h -- 打砖块 subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / sudoku
 * subpages: the page group lives inside the interaction tile (NOT a
 * top-level tile), and this module owns only what is inside that group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state (muse_breakout.c);
 * there is no worker and no NVS here -- coin movement is two commands
 * to the pet worker, PET_FEE at start and PET_PLAY_REWARD once at OVER
 * (see below).
 *
 * Economy (same as snake / whack-a-mole / goldrain / doodle): each
 * game costs 10 coins. Tapping 开始 on the START overlay or 再来一局
 * on the OVER overlay (which prints "- 10 金币" under the label)
 * first checks the pet snapshot: under 10 coins the start is refused
 * with "金币不够（需 10 枚）" -- the short copy, because the note
 * shares the slot under the field, where only ~213 px of chord is
 * left -- and nothing is charged; otherwise PET_FEE is queued and the
 * game starts. Resume after pause never charges again, and there is
 * no restart button on the pause card (same decision as snake: one tap
 * in the bezel must not spend 10 coins). The worker is the only writer
 * of coins; if its re-check finds the snapshot was stale (PET_FEE
 * feedback PET_POOR), the interact tile calls
 * muse_breakout_ui_fee_refused(), which cancels a running game back to
 * START with the refusal note.
 *
 * Score and payout: one brick is one point, 24 bricks is a full
 * board, and there is no penalty item, so the score only ever goes up.
 * The round's score is converted exactly once, at OVER, by
 * muse_pet_round_coins() -- 1 point per coin, 20 coins per round
 * ceiling -- then sent as PET_PLAY_REWARD. The payout is gross: the
 * 10-coin entry fee was already charged at start, so a full board
 * nets 20 - 10 = +10, and the 10-point break-even matches the other
 * games. The result is printed once, under the field: white and
 * score-only ("本局 n 分") while PLAYING or PAUSED -- there is no
 * countdown, the round ends on bricks or lives, not on a clock -- and
 * GOLD with the gross payout ("本局 n 分，+m 金币") at OVER (same slot
 * as the snake page). The net is deliberately NOT printed (user: kids
 * cannot tell net apart) and the OVER card carries no line of its own
 * (the page title becomes "游戏结束"). Leaving the page mid-round
 * pauses and settles nothing.
 *
 * Controls (user decision 2026-11-18): tilt is the primary control --
 * the accelerometer (muse_imu.c, official waveshare/qmi8658 component,
 * polled once per 50 ms tick, no interrupts) moves the paddle left
 * and right, with the 金币雨 calibration (sign -1, gain 40, dead zone
 * 0.5, smoothing 0.35) and a 260 px/s glide cap so it can still catch
 * a ball headed for a bottom corner at the vx cap of 220 px/s; finger
 * drag on the field is the backup (a tap jumps the paddle under the
 * finger, 1:1, uncapped). Pausing is a LONG HOLD on the field: the
 * finger stays within ~12 px for 500 ms. The hold is tracked by hand,
 * not LVGL's LONG_PRESSED: a drag that happens to linger would
 * otherwise pause mid-drag, and the pause must never fire from a
 * moving finger. There is no pause button (same as snake /
 * whack-a-mole since 2026-11-01: the old 34 px one sat outside the
 * round bezel). The back arrow stays hidden while playing -- hold
 * first, then leave from the pause card.
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_breakout_ui_hide) pauses a running game, so a return visit
 * resumes it from the pause overlay; a reboot restarts from START.
 * The game never touches voice: screen taps are consumed by the touch
 * layer, and BOOT keeps its global behaviour (it is not a game key),
 * exactly like the other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Build the 打砖块 page inside `page` (a 360x360 group created
 * by muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: 打砖块 back always lands on the
 * mini-game menu). Safe to call once, in the LVGL task. */
void muse_breakout_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: refreshes the board from the kept game state and
 * wakes the accelerometer. Progress is NOT reset: a paused game stays
 * paused, a finished game stays on its OVER overlay, an unstarted game
 * stays on START. */
void muse_breakout_ui_show(void);

/* Leaving the tile: pauses a running game (timer stopped, pause
 * overlay shown) and sleeps the accelerometer, so neither plays on
 * invisibly. Any other state is left as-is, so a return visit resumes
 * exactly where it left off. */
void muse_breakout_ui_hide(void);

/* PET_FEE feedback came back PET_POOR (the snapshot gate was stale
 * and the fee never settled): cancel a running game back to START
 * with the refusal note. Called from the interact tile's feedback
 * drain, on the LVGL task. */
void muse_breakout_ui_fee_refused(void);

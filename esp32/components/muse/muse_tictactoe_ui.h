/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tictactoe_ui.h -- OX棋 subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / sudoku
 * subpages: the page group lives inside the interaction tile (NOT a
 * top-level tile), and this module owns only what is inside that group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state (muse_tictactoe.c);
 * there is no worker and no NVS here -- coin movement is two commands
 * to the pet worker, PET_FEE at start and PET_PLAY_REWARD once at OVER
 * (see below).
 *
 * Economy (the RPS model, user decision 2026-11-08): a round costs
 * 10 coins. Tapping 开始 on the START overlay or 再来一局 on the
 * OVER overlay (which prints "- 10 金币" under the label) first checks
 * the pet snapshot: under 10 coins the start is refused with
 * "金币不够（需 10 枚）" and nothing is charged; otherwise PET_FEE is
 * queued and the game starts. Resume after pause never charges again,
 * and there is no restart button on the pause card (same decision as
 * snake: one tap in the bezel must not spend 10 coins). The worker is
 * the only writer of coins; if its re-check finds the snapshot was
 * stale (PET_FEE feedback PET_POOR), the interact tile calls
 * muse_tictactoe_ui_fee_refused(), which cancels a running game back
 * to START with the refusal note.
 *
 * Score and payout: the round ends win / draw / loss, settled exactly
 * once at OVER by muse_tictactoe_payout_coins() -- win 20, draw 10,
 * loss 0 -- then sent as PET_PLAY_REWARD. The payout is gross: the
 * 10-coin entry fee was already charged at start, so the round nets
 * +10 / 0 / -10, exactly the rock-paper-scissors rule. The result is
 * printed once, under the field: "轮到你了" while PLAYING or PAUSED,
 * GOLD with the gross payout ("+m 金币") at OVER. The net is
 * deliberately NOT printed (user: kids cannot tell net apart). The
 * title slot itself becomes the result at OVER ("你赢了！" gold,
 * "平局", "你输了" -- mockup v4, user decision 2026-11-08) instead of
 * the other games' "游戏结束": a single-round game needs the result
 * named, not just the payout. Leaving the page mid-round pauses and
 * settles nothing.
 *
 * Controls (user decision 2026-11-08, mockup v4): tap a cell to place
 * X -- the player is X and moves first, the AI is O and answers after
 * a 500 ms beat so the player's mark is seen before the answer lands.
 * Pausing is a LONG HOLD anywhere on the board: the finger stays
 * within ~12 px for 500 ms (the goldrain / mathpractice idiom). The
 * hold is tracked by hand, not LVGL's LONG_PRESSED, for the same
 * reason as goldrain: the tap and the hold share the board, so the
 * placement fires on release-after-a-short-press, never on
 * press-down -- a hold-to-pause never places a piece, which is what
 * resolves the centre-cell conflict (tapping the centre places a
 * piece, holding it pauses; the two are mutually exclusive by
 * duration). There is no pause button (same as snake / whack-a-mole
 * since 2026-11-01). The back arrow stays hidden while playing --
 * hold first, then leave from the pause card.
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_tictactoe_ui_hide) pauses a running game, so a return visit
 * resumes it from the pause overlay; a reboot restarts from START.
 * The game never touches voice: screen taps are consumed by the touch
 * layer, and BOOT keeps its global behaviour (it is not a game key),
 * exactly like the other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Build the OX棋 page inside `page` (a 360x360 group created
 * by muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: OX棋 back always lands on the
 * mini-game menu). Safe to call once, in the LVGL task. */
void muse_tictactoe_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: refreshes the board from the kept game state.
 * Progress is NOT reset: a paused game stays paused, a finished game
 * stays on its OVER overlay, an unstarted game stays on START. */
void muse_tictactoe_ui_show(void);

/* Leaving the tile: pauses a running game (timer stopped, pause
 * overlay shown), so it never plays on invisibly. Any other state is
 * left as-is, so a return visit resumes exactly where it left off. */
void muse_tictactoe_ui_hide(void);

/* PET_FEE feedback came back PET_POOR (the snapshot gate was stale
 * and the fee never settled): cancel a running game back to START
 * with the refusal note. Called from the interact tile's feedback
 * drain, on the LVGL task. */
void muse_tictactoe_ui_fee_refused(void);

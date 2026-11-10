/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_mathpractice_ui.h -- 加减练习 subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune subpages:
 * the page group lives inside the interaction tile (NOT a top-level
 * tile), and this module owns only what is inside that group.
 *
 * 2026-11-08 (user decision): the separate 凑十法练习 page was fused
 * into this one. One game, two kinds of question -- 个位数相加（凑十法）
 * and 十几减个位（借十法）-- three comparison buttons (> < =), and a
 * verdict line that says 正确，+1 分 / 错误，不得分 after every pick.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state
 * (muse_mathpractice.c); there is no worker and no NVS here -- coin
 * movement is two commands to the pet worker, PET_FEE at start and
 * PET_PLAY_REWARD once at OVER (see below).
 *
 * Economy (same as snake / whack-a-mole / goldrain / doodle): each
 * game costs 5 coins. Tapping 开始 on the START overlay or 再来一局 on
 * the OVER overlay (which prints "- 5 金币" under the label) first
 * checks the pet snapshot: under 5 coins the start is refused with
 * "金币不够（需 5 枚）" and nothing is charged; otherwise PET_FEE is
 * queued and the game starts. Resume after pause never charges again,
 * and there is no restart button on the pause card (same decision as
 * snake: one tap in the bezel must not spend 5 coins). The worker is
 * the only writer of coins; if its re-check finds the snapshot was
 * stale (PET_FEE feedback PET_POOR), the interact tile calls
 * muse_mathpractice_ui_fee_refused(), which cancels a running game
 * back to START with the refusal note.
 *
 * Score and payout: a right pick is +1, a wrong one is +0, so the
 * score only ever goes up. The 2-minute round's score is converted exactly
 * once, at OVER, by muse_pet_round_coins_cap() -- 1 point per coin, at
 * most 20 coins per round (this game's own ceiling since the round grew
 * to 3 minutes on 2026-11-08; every other game stays at 10) -- then sent
 * as PET_PLAY_REWARD. The payout
 * is gross: the 5-coin entry fee was already charged at start, so no
 * round mints more than 5 coins. The HUD reads "本局 n 分 · 剩余 s 秒"
 * while PLAYING or PAUSED and turns GOLD with "本局 n 分，+m 金币" at
 * OVER. The net is deliberately NOT printed (kids cannot tell net
 * apart) and the OVER card carries no line of its own (the page title
 * becomes "游戏结束"). Leaving the page mid-round pauses and settles
 * nothing.
 *
 * Controls (user decision 2026-11-08): one question at a time -- an
 * addition with a carry ("8 + 5 ? 13") or a teen-minus-single-digit
 * that borrows ten ("14 - 6 ? 8") -- and three buttons: > at x=30, <
 * at x=135, = at x=240. The player taps the comparison that makes the
 * statement true. A right pick is +1, a wrong one +0, and the verdict
 * ("正确，+1 分" / "错误，不得分") holds for 600 ms; the next question
 * is dealt when it ends, and taps during the window are ignored, so a
 * fast double tap cannot skip a question. Pausing is a LONG HOLD on
 * the question area (finger within ~12 px for 500 ms, the doodle
 * page's idiom): the buttons must never pause the game, so the hold is
 * tracked on a layer under them. There is no pause button (same as
 * snake / whack-a-mole since 2026-11-01). The back arrow stays hidden
 * while playing -- hold first, then leave from the pause card.
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_mathpractice_ui_hide) pauses a running game, so a return visit
 * resumes it from the pause overlay; a reboot restarts from START. The
 * game never touches voice: screen taps are consumed by the touch
 * layer and the buttons, and BOOT keeps its global behaviour (it is
 * not a game key), exactly like the other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Build the 加减练习 page inside `page` (a 360x360 group created by
 * muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: 加减练习 back always lands on the
 * mini-game menu). Safe to call once, in the LVGL task. */
void muse_mathpractice_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: refreshes the board from the kept game state.
 * Progress is NOT reset: a paused game stays paused, a finished game
 * stays on its OVER overlay, an unstarted game stays on START. */
void muse_mathpractice_ui_show(void);

/* Leaving the tile: pauses a running game (timer stopped, pause
 * overlay shown), so it never plays on invisibly. Any other state is
 * left as-is, so a return visit resumes exactly where it left off. */
void muse_mathpractice_ui_hide(void);

/* PET_FEE feedback came back PET_POOR (the snapshot gate was stale and
 * the fee never settled): cancel a running game back to START with the
 * refusal note. Called from the interact tile's feedback drain, on the
 * LVGL task. */
void muse_mathpractice_ui_fee_refused(void);

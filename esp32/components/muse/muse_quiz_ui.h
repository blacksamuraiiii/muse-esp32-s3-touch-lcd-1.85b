/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_quiz_ui.h -- 英语口语（听力练习）subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune subpages:
 * the page group lives inside the interaction tile (NOT a top-level
 * tile), and this module owns only what is inside that group.
 *
 * 2026-11-11 (user decision): the word-cards "考一考" is this page --
 * hear the English word, pick its picture from three options. No
 * START page and no pause (the word-cards shape, user 2026-11-10):
 * entering the page starts a round at once and the back arrow is the
 * only way out; leaving mid-round pauses the UI timer (the clock
 * stops with it) and coming back resumes it -- the round never ends
 * by itself while the player is away.
 *
 * A round is 5 questions in 60 seconds (user 2026-11-11, was 120).
 * Free entry: no fee is charged at start, so there is no fee-refused
 * path and no price on the 再来一局 button. A right pick is +1, a
 * wrong one is +0, so the score only ever goes up; at OVER it is
 * converted exactly once by muse_pet_round_coins_cap() -- 1 point per
 * coin, at most 20 coins per round (the shared ceiling; a round tops
 * out at 5) -- then sent as PET_PLAY_REWARD. The HUD reads
 * "本局 n 分 · 剩余 s 秒" while PLAYING and turns GOLD with
 * "本局 n 分，+m 金币" at OVER. The net is deliberately NOT printed
 * (kids cannot tell net apart) and the OVER card carries no line of
 * its own (the page title stays "英语口语").
 *
 * The verdict line (y=280, the math game's slot) reads "第 n/5 题"
 * while a question is open and, for 600 ms after a pick, "✓ 答对了，
 * +1 分" in green or "✗ 答错了，不得分" in red (LV_SYMBOL_OK /
 * LV_SYMBOL_CLOSE, the math game's icons). The picked card takes a
 * pressed fill and a 3 px GOLD (right) / DANGER (wrong) ring for the
 * same window; only the picked card is ringed, the right answer is
 * not shown. Taps during the window are ignored, so a fast double
 * tap cannot skip a question.
 *
 * Pronunciation reuses the word-cards pipeline (muse_wordcards_audio):
 * entering the page prefetches the round's 5 answer words into PSRAM
 * (the quiz's own slot pool); the speaker button replays the current
 * question's word from that cache. The 50 ms tick drains the logic's
 * pending_audio mark -- when READY it plays and the mark is cleared;
 * when still FETCHING the mark waits for the next tick; when FAILED
 * it is dropped silently (the question still shows, nothing blocks).
 * A word whose fetch failed simply stays silent.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state (muse_quiz.c);
 * there is no worker and no NVS here -- coin movement is one command
 * to the pet worker, PET_PLAY_REWARD once at OVER (see below).
 */
#pragma once

#include "lvgl.h"

/* Build the page's objects (once). The game starts on ui_show(). */
void muse_quiz_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: START starts a round at once (and prefetches
 * its 5 answer words); PLAYING prefetches again (READY words are
 * kept) and resumes the clock; OVER stays. */
void muse_quiz_ui_show(void);

/* Leaving the page: pause the clock, stop any playback, abort any
 * prefetch in flight, drop a pending pronunciation. The phase is
 * left alone, so coming back resumes the round. */
void muse_quiz_ui_hide(void);

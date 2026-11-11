/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards_ui.h -- 英语单词卡片 subpages inside the interaction tile.
 *
 * Two pages, three levels (user decision 2026-11-10, phase 1: fruit
 * + animal, 8 cards; phase 2 2026-11-11 adds things; the same day the two
 * animal sets merged into one 8-word group over two swiped pages): the
 * minigame list's "单词练习" row opens the category page
 * (muse_wordcards_ui_build_sets: one row per offered set -- "水果 ·
 * Fruit", "动物 · Animal", "食物 · Food", "物品 · Things" --
 * icon + title centred as one group, no count text, hint "限时免费"
 * as the list's last item), and a
 * category row opens that set's 2x2 card page (muse_wordcards_ui_build).
 * Back goes one level up at a time: card page -> category page ->
 * minigame list. Entering a set always starts it fresh (START, all
 * cards front-up, prefetch of that set's 4 MP3s); leaving a set
 * resets its flipped cards. There is no time limit (user decision
 * 2026-11-11): no countdown, no summary, no pause screen.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / OX棋
 * subpages: the page groups live inside the interaction tile (NOT a
 * top-level tile), and this module owns only what is inside that group.
 *
 * Threading follows the interact contract: LVGL callbacks only touch
 * this module's own objects and the RAM game state (muse_wordcards.c);
 * the audio prefetch and playback live on their own tasks
 * (muse_wordcards_audio.c) and only move the cache bookkeeping.
 * No coins move here: the game is a free learning mode (user decision
 * 2026-11-10), so there is no entry fee, no PET_FEE and no payout --
 * nothing in this module touches the economy.
 *
 * Controls (user decision 2026-11-10, mockup v4; pause dropped the same
 * day, the 120 ms squash dropped 2026-11-11 because it needed a 63.5 KB
 * layer buffer per card and crashed the renderer): tap a card's front to
 * flip it instantly -- and
 * the back shows the English word (gold, Montserrat 20), the Chinese
 * (Noto 16) and the speaker logo (~31x18, v3 size), and the
 * pronunciation plays automatically from the PSRAM cache. Tap the
 * speaker logo to replay; tap anywhere else on a flipped card to flip
 * it back. There is no pause: no long hold, no pause card, and the back
 * arrow is always visible top-left (it is the only way out of a round
 * that never ends by itself).
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_wordcards_ui_hide) stops the timer and any playback, so it
 * never plays on invisibly; a reboot restarts from START. The game
 * never touches voice: screen taps are consumed by the touch layer,
 * and BOOT keeps its global behaviour (it is not a game key), exactly
 * like the other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Callback the category page calls when a set row is tapped: the
 * owner (muse_interact_ui.c) shows the card page for that set. */
typedef void (*muse_wordcards_open_set_cb_t)(int set);

/* Build the category page inside `page` (a 360x360 group created by
 * muse_interact_ui.c). `back_cb` is the back-arrow handler (back to
 * the minigame list); `open_cb` runs when a set row is tapped.
 * One row per offered set, in table order. Safe to call once, in
 * the LVGL task. */
void muse_wordcards_ui_build_sets(lv_obj_t *page, lv_event_cb_t back_cb,
                                  muse_wordcards_open_set_cb_t open_cb);

/* Switch the card page to set `set` for a fresh round (START, all
 * cards front-up, cache cleared): the card pictures and back texts
 * are rebuilt from the new set's table, and the prefetch for the
 * new set starts when muse_wordcards_ui_show() runs. No-op when
 * the set has no art. Called by the owner before showing the card
 * page; safe in the LVGL task. */
void muse_wordcards_ui_open_set(int set);

/* Build the word-cards page inside `page` (a 360x360 group created
 * by muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: word-cards back always lands on the
 * category page). Safe to call once, in the LVGL task. */
void muse_wordcards_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: refreshes the cards from the kept game state and
 * starts prefetching this set's 4 MP3s into PSRAM, so a flip plays
 * from the cache at once. Progress is NOT reset: a paused round stays
 * paused, a finished round stays on its OVER overlay, an unstarted
 * round stays on START. */
void muse_wordcards_ui_show(void);

/* Leaving the tile: pauses a running round (timer stopped, pause
 * overlay shown), stops any playback and aborts a prefetch in flight,
 * so nothing plays or fetches invisibly. Any other state is left
 * as-is, so a return visit resumes exactly where it left off. */
void muse_wordcards_ui_hide(void);

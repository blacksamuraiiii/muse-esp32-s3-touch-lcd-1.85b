/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards_ui.h -- 英语单词卡片 subpage inside the interaction tile.
 *
 * Owned by muse_interact_ui.c like the feed / game / fortune / OX棋
 * subpages: the page group lives inside the interaction tile (NOT a
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
 * Controls (user decision 2026-11-10, mockup v4): tap a card's front
 * to flip it -- a 120 ms horizontal squash, swap, expand (not a true
 * 3D flip, LVGL cannot do that cheaply) -- and the back shows the
 * English word (gold, Montserrat 20), the Chinese (Noto 16) and the
 * speaker logo (~31x18, v3 size), and the pronunciation plays
 * automatically from the PSRAM cache. Tap the speaker logo to replay;
 * tap anywhere else on a flipped card to flip it back. Pausing is a
 * LONG HOLD anywhere on the page: the finger stays within ~12 px for
 * 500 ms (the goldrain / mathpractice / OX棋 idiom). The hold is
 * tracked by hand, not LVGL's LONG_PRESSED: the tap and the hold
 * share the cards, so the flip fires on release-after-a-short-press,
 * never on press-down -- a hold-to-pause never flips a card. There is
 * no pause button. The back arrow stays hidden while playing -- hold
 * first, then leave from the pause card.
 *
 * Progress lives in static RAM: leaving the tile
 * (muse_wordcards_ui_hide) pauses a running round and stops any
 * playback, so it never plays on invisibly; a reboot restarts from
 * START. The game never touches voice: screen taps are consumed by
 * the touch layer, and BOOT keeps its global behaviour (it is not a
 * game key), exactly like the other mini-game pages.
 */
#pragma once

#include "lvgl.h"

/* Build the word-cards page inside `page` (a 360x360 group created
 * by muse_interact_ui.c). `back_cb` is the back-arrow handler (the
 * interaction tile's on_back: word-cards back always lands on the
 * mini-game menu). Safe to call once, in the LVGL task. */
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

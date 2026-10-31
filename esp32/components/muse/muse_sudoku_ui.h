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
 * there is no worker and no NVS here -- the only coin movement is the
 * 5-coin entry fee per puzzle, sent as PET_FEE (see the .c file);
 * Sudoku never pays coins out. Progress lives in static RAM: leaving
 * the tile (muse_sudoku_ui_hide) closes the pad popup and the win overlay
 * but keeps the puzzle and its paid flag, so re-entering the page resumes
 * it without charging again; a reboot restarts puzzle #01.
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

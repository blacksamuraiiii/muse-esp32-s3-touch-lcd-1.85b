/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_ui.h -- 1.5a interaction tile + six subpages.
 *
 * Built by muse_ui_start() on the interaction tile (0,0) of the 3-tile
 * layout (interaction, face, settings). This file owns NO tileview, NO
 * dots and NO caption label: muse_ui.c owns those (see muse_ui_patch.md).
 * It only owns the content of the interaction tile and its six subpages
 * (feed, game, fortune, sudoku, minigame, snake), which are groups inside that tile -- NOT top-level
 * tiles, so the bottom dots never change when a subpage opens.
 *
 * Threading, exactly as design §3.1: LVGL callbacks here only SEND actions
 * (muse_pet_request()) and NEVER settle state; the pet worker is the only
 * writer. muse_interact_ui_tick() runs in the LVGL task and is the only
 * place that drains the feedback queue for UI purposes. It must be called
 * from the LVGL frame tick on every frame, whether or not the interaction
 * tile is currently visible, so feedback is never left to block the worker
 * (phase-1: worker blocks on a full feedback queue) and so the global
 * pet caption can expire even after the user swipes away.
 *
 * User-visible copy never contains "宠物" or "PET" (user decision: MaoMao
 * is not a pet). Code symbols keep the muse_pet_* names from TamaMe for
 * line-by-line comparison; that is a code naming choice, not UI copy.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Build the interaction tile content. `tile` is the (0,0) tile object
 * created by muse_ui.c. Safe to call once, in the LVGL task, after the
 * tile exists and before the frame timer starts ticking it. */
void muse_interact_ui_build(lv_obj_t *tile);

/* One LVGL-tick of work. `active` is true while the interaction tile is
 * the visible tile (muse_ui.c passes lv_tileview_get_tile_active() ==
 * its interaction tile). Always call it, even when !active: feedback
 * draining, caption expiry and decay-notice announcement depend on it.
 * It never blocks, never waits on the worker, and never holds a power
 * lock (design §6.3: interaction must not prevent light sleep). */
void muse_interact_ui_tick(bool active);

/* Called by muse_ui.c when the visible tile stops being the interaction
 * tile. Clears the result note, cancels any in-flight game animation and
 * closes all subpages back to the interaction main page, so a
 * return visit never resurrects a stale result (design §5.2). It does
 * NOT clear the global pet caption: the caption is allowed to outlive
 * the tile visit and expires on its own 3-second rule. */
void muse_interact_ui_on_hidden(void);

/* Called by muse_ui.c's on_canvas_clicked() when a face tap could not
 * even queue PET_TOUCH (worker not ready). The tick turns this flag into
 * the "互动未就绪" caption on the next IDLE frame; it is not written
 * directly from the tap callback, so the caption path stays single-owner. */
void muse_interact_ui_touch_failed(void);

#if CONFIG_MUSE_BUILD_TEST
/* Bench command "ui.game <0|1|2>": starts one round on the game subpage the
 * way a tap would, so the shake/reveal path can be driven from the console.
 * Callable from any task: it queues the round for the LVGL tick. Test builds
 * only -- it is not part of the product surface. */
bool muse_interact_ui_test_game(int choice);
#endif

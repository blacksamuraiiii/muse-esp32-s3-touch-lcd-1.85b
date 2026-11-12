/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_main_grid_art.h -- interaction main-page 2x2 grid icon masters
 * (1.5.3a A flow).
 *
 * Source (user decision 2026-11-11, same set as the word cards):
 *   game-icons/icons repository, CC BY 3.0 -- keep the Game-icons
 *   attribution in the release notes.
 *     crystal_ball = lorc/crystal-ball.svg     (Lorc, 今日运势)
 *     cupcake      = delapouite/cupcake.svg    (Delapouite, 喂饭, user pick)
 *     spell_book   = delapouite/spell-book.svg (Delapouite, 学习, user pick)
 *     gamepad      = delapouite/gamepad.svg    (Delapouite, 小游戏)
 *   All four are 512x512 single-colour SVGs, drawn white
 *   (MUSE_THEME_TEXT) on the grid cards -- no per-icon colour.
 *
 * Generated once, offline, by ~/workspace/main-grid-art-gen.py; the
 * four .c files beside this header hold the pixel data (LVGL A8,
 * 128x128 master). Do not hand-edit them and do not hand-draw
 * replacements: the only approved art is the set above.
 *
 * Format is A8, not ARGB8888: one ARGB8888 128x128 master is 64 KB,
 * and four icons would be 256 KB against the ~290 KB free in the
 * 4 MB app slot at 1.5.2a. A8 is 16 KB per icon (64 KB for the
 * grid). The UI recolours the mask white at runtime
 * (lv_obj_set_style_image_recolor + _opa COVER), exactly like the
 * word-cards art.
 *
 * Regenerate:
 *   1. take each SVG, delete its first full-canvas path
 *      (<path d="M0 0h512v512H0z"/>), which would render as a black square;
 *   2. render 256x256 PNG, white on transparent, then Lanczos-resize
 *      to 128x128 (one master, LVGL scales it at runtime);
 *   3. python3 ~/workspace/main-grid-art-gen.py
 *      (writes the four .c files; this header's list is by hand).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_main_grid_art_crystal_ball;
extern const lv_image_dsc_t muse_main_grid_art_cupcake;
extern const lv_image_dsc_t muse_main_grid_art_spell_book;
extern const lv_image_dsc_t muse_main_grid_art_gamepad;

/* Master size, px (square). The UI draws the picture at 56 px. */
#define MUSE_MAIN_GRID_ART_PX 128

/* Grid slot -> descriptor, in main-page order
 * (今日运势, 喂饭, 学习, 小游戏). */
enum {
    MUSE_MAIN_GRID_FORTUNE = 0,
    MUSE_MAIN_GRID_FEED,
    MUSE_MAIN_GRID_LEARN,
    MUSE_MAIN_GRID_GAMES,
    MUSE_MAIN_GRID_COUNT,
};

extern const lv_image_dsc_t *const MUSE_MAIN_GRID_ART[MUSE_MAIN_GRID_COUNT];

#ifdef __cplusplus
}
#endif

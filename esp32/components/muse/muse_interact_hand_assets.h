/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_hand_assets.h -- the three Game-icons hand images.
 *
 * Source (user decision 2026-10-25, design 5.5):
 *   game-icons/icons repository, john-redman/rock.svg, paper.svg,
 *   scissors.svg -- John Redman, CC BY 3.0, 512x512 grid, single colour.
 *   Keep the Game-icons attribution in the 1.5a release notes (CC BY 3.0).
 *   Order in the game subpage is rock, paper, scissors (left to right).
 *
 * Generated once, offline; the three .c files beside this header hold the
 * pixel data (LVGLImage.py, ARGB8888, 128x128 master). Do not hand-edit them
 * and do not hand-draw replacements: the only approved art is the set above.
 *
 * Regenerate:
 *   1. take each SVG, delete its first full-canvas path
 *      (<path d="M0 0h512v512H0z"/>), which would render as a black square;
 *   2. wrap the rest in <g transform="rotate(-90 256 256)"> so the hands
 *      point UP, and render 256x256 PNG, white on transparent, then
 *      Lanczos-resize to 128x128 (one master, LVGL scales it at runtime);
 *   3. python3 LVGLImage.py --ofmt C --cf ARGB8888 --name muse_interact_hand_<x>
 *      and copy the .c here.
 *
 * The loser-dim effect is NOT a second asset: the UI recolours the same image
 * at runtime (lv_image_set_recolor). The winner border is a container border.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_interact_hand_rock;
extern const lv_image_dsc_t muse_interact_hand_paper;
extern const lv_image_dsc_t muse_interact_hand_scissors;

#ifdef __cplusplus
}
#endif

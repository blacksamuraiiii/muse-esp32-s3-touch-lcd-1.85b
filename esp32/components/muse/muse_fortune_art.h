/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_fortune_art.h -- 今日运势子页 two-card colour icons (mockup v13).
 *
 * Source (user decision 2026-11-14, mockup v13):
 *   qiantong = web candidate C (135editor), picked by the user from 4
 *              candidates; flat red tube + yellow sticks, with the gold
 *              签 overlaid on the tube (y=136). The 签's small print
 *              comes with the original image.
 *   jiaobei  = the holy pair, left red-convex + right flat wood with
 *              shading (mockup v12/v13), redrawn from the mockup
 *              geometry -- not the flat blue crescent pair from v3.
 *
 * Format is RGB565A8, not the A8 single-colour mask the learn cards
 * use: both icons are genuinely multi-colour, and an A8 mask can only
 * carry one colour per card. The two icons are 2 x 80x80x3 = 38.4 KB
 * of flash (stored at the 80 px display size, so the UI draws them
 * with no recolour and no scaling). Converted 2026-11-20 (size-trim)
 * from the ARGB8888 masters: RGB565 colour plane + A8 alpha plane
 * (alpha byte-identical, colour quantised 5/6/5). Plain RGB565 was
 * NOT used: both icons rely on per-pixel alpha (transparent corners
 * on the card, and the jiaobei shading), which RGB565 would paint
 * black. See ~/workspace/fortune-art-gen.py
 * for the generator; the two .c files beside this header hold the
 * pixel data. Do not hand-edit them.
 *
 * Attribution: the qiantong image is a 135editor web graphic used as-is
 * at the user's direction (2026-11-14, "网上找图标直接用吧"); keep a
 * note of that source in the release notes. The jiaobei pair is drawn
 * in-house from the mockup.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_fortune_art_qiantong;
extern const lv_image_dsc_t muse_fortune_art_jiaobei;

/* Icon size, px (square). The UI draws the picture at 80 px. */
#define MUSE_FORTUNE_ART_PX 80

/* Card slot -> descriptor, in subpage order (抽签, 掷筊). */
enum {
    MUSE_FORTUNE_QIAN = 0,
    MUSE_FORTUNE_JIAOBEI,
    MUSE_FORTUNE_COUNT,
};

extern const lv_image_dsc_t *const MUSE_FORTUNE_ART[MUSE_FORTUNE_COUNT];

#ifdef __cplusplus
}
#endif

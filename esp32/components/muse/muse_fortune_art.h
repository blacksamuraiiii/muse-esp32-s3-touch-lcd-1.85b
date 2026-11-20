/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_fortune_art.h -- 今日运势子页 three-card colour icons (mockup v13
 * + lucky wheel v8, 2026-11-20).
 *
 * Source (user decision 2026-11-14, mockup v13):
 *   qiantong = web candidate C (135editor), picked by the user from 4
 *              candidates; flat red tube + yellow sticks, with the gold
 *              签 overlaid on the tube (y=136). The 签's small print
 *              comes with the original image.
 *   jiaobei  = the holy pair, left red-convex + right flat wood with
 *              shading (mockup v12/v13), redrawn from the mockup
 *              geometry -- not the flat blue crescent pair from v3.
 *   luckywheel = the wheel itself, shrunk from the v7 wheel page art
 *              (user decision 2026-11-20): 12 segments, gold ring, gold
 *              centre dot, no labels, no pointer.
 *
 * Format is ARGB8888, not the A8 single-colour mask the learn cards
 * use: all three icons are genuinely multi-colour, and an A8 mask can only
 * carry one colour per card. The qiantong and jiaobei icons are 2 x
 * 80x80x4 = 51.2 KB of flash (stored at the 80 px display size, so the
 * UI draws them with no recolour and no scaling); the wheel icon is
 * 64x64x4 = 16 KB. The subpage draws all three at 64 px: the two 80 px
 * icons are scaled down with lv_image_set_scale (204/256), the wheel
 * icon is drawn at its native 64 px. See ~/workspace/fortune-art-gen.py
 * and ~/workspace/luckywheel-art-gen.py for the generators; the .c files
 * beside this header hold the pixel data. Do not hand-edit them.
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
extern const lv_image_dsc_t muse_luckywheel_art_icon;

/* Icon size, px (square). The two 80 px icons are drawn at 64 px on the
 * subpage (scale 204); the wheel icon is stored at 64 px and drawn
 * at its native size. */
#define MUSE_FORTUNE_ART_PX 80
#define MUSE_FORTUNE_ART_DISPLAY_PX 64

/* Card slot -> descriptor, in subpage order (抽签, 掷筊, 幸运转盘). */
enum {
    MUSE_FORTUNE_QIAN = 0,
    MUSE_FORTUNE_JIAOBEI,
    MUSE_FORTUNE_LUCKYWHEEL,
    MUSE_FORTUNE_COUNT,
};

extern const lv_image_dsc_t *const MUSE_FORTUNE_ART[MUSE_FORTUNE_COUNT];

#ifdef __cplusplus
}
#endif

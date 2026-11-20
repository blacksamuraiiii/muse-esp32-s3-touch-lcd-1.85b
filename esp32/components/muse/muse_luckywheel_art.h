/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_luckywheel_art.h -- art for the lucky wheel page (see the .c files
 * for the generator note). The wheel is RGB565 (115,200 B); the icon and
 * pointer are ARGB8888.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_luckywheel_art_wheel;    /* 240x240 RGB565 */
extern const lv_image_dsc_t muse_luckywheel_art_icon;     /* 64x64 ARGB8888 */
extern const lv_image_dsc_t muse_luckywheel_art_pointer;  /* 28x18 ARGB8888 */

#define MUSE_LUCKYWHEEL_ART_WHEEL_PX 240
#define MUSE_LUCKYWHEEL_ART_ICON_PX 64
#define MUSE_LUCKYWHEEL_ART_POINTER_W 28
#define MUSE_LUCKYWHEEL_ART_POINTER_H 18

#ifdef __cplusplus
}
#endif

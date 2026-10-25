/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_food_art.h -- 1.5a feed-subpage food pixel art.
 *
 * Verbatim 16x16 grids for three foods, copied from TamaMe
 * (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet_art.c, Apache-2.0;
 * TamaMe attribution retained in muse_pet_model.h and in the design doc).
 * Upstream icon indices: 3 = coffee, 4 = cake, 5 = rice. Only these three
 * are carried into 1.5a; hotpot (6), chicken (7) and tea (8) are dropped
 * by user decision (chicken reads as a brown blob at 80px, tea is another
 * cup like coffee, hotpot is too dark) and must NOT reappear here.
 *
 * Colour map is upstream muse_pet_art_pixel()'s switch, verbatim:
 *  y = 0xA76A19, Y = 0xF6CB4B, W = 0xFFF2CE, O = 0x644331,
 *  b = 0xCB934F, G = 0x79AC69, R = 0xBD6655, P = 0xB5A4E7,
 *  C = 0x7DADCB, D = 0x51637F; space = transparent (do not draw).
 * Our three grids only use W, O, R, b, G -- the full map is kept so a
 * future food cannot silently change colour by reusing a letter.
 *
 * Rendering (see muse_interact_ui.c, draw_food_cb): each grid cell is one
 * LVGL rectangle, scale = icon_px / 16 = 5 at the 80px design size, nearest
 * neighbour (no smoothing -- smoothing is what made the earlier pixel
 * versions look muddy). The draw callback runs in the LVGL draw pass, so
 * it must not allocate, queue, or call the pet worker.
 */
#pragma once

#include <stdint.h>

#define MUSE_INTERACT_FOOD_PX 16u

typedef struct {
    const char *name;              /* UI label under the button */
    const char *rows[MUSE_INTERACT_FOOD_PX];
} muse_interact_food_art_t;

/* Order is the feed-subpage button order: rice, cake, coffee. */
static const muse_interact_food_art_t MUSE_INTERACT_FOODS[3] = {
    {
        "米饭",
        {
            "                ",
            "       W        ",
            "     W   W      ",
            "                ",
            "    WWWWWWWW    ",
            "   WOOOOGOWWW   ",
            "  WOOOGOOOOWWW  ",
            " WWWWOOOWWWWWWW ",
            " OOOOOOOOOOOOOO ",
            "  RRRRRRRRRRRR  ",
            "  RRRRRRRRRRRR  ",
            "   RRRRRRRRRR   ",
            "    RRRRRRRR    ",
            "     RRRRRR     ",
            "                ",
            "                ",
        },
    },
    {
        "蛋糕",
        {
            "                ",
            "       R        ",
            "      RRR       ",
            "   WWWWWWWWWW   ",
            "  WWWWWWWWWWWW  ",
            "  bbbbbbbbbbbb  ",
            "  WWWWWWWWWWWW  ",
            "  WWWWWWWWWWWW  ",
            "  bbbbbbbbbbbb  ",
            "  WWWWWWWWWWWW  ",
            "  WWWWWWWWWWWW  ",
            "  OOOOOOOOOOOO  ",
            "  OOOOOOOOOOOO  ",
            " WWWWWWWWWWWWWW ",
            "                ",
            "                ",
        },
    },
    {
        "咖啡",
        {
            "    W    W      ",
            "     W  W       ",
            "                ",
            "  WWWWWWWWW     ",
            "  WWWWWWWWWWWW  ",
            "  WWOOOOOWW  W  ",
            "  WWWWWWWWW  W  ",
            "  WWWWWWWWWWWW  ",
            "  WWWWWWWWW     ",
            "  WWWWWWWWW     ",
            "   WWWWWWW      ",
            "    WWWWW       ",
            "                ",
            " WWWWWWWWWWWW   ",
            "                ",
            "                ",
        },
    },
};

/* Pixel letter -> RGB888, 0 = transparent. Matches upstream exactly. */
static inline uint32_t muse_interact_food_pixel_color(char c)
{
    switch (c) {
    case 'y': return 0xA76A19;
    case 'Y': return 0xF6CB4B;
    case 'W': return 0xFFF2CE;
    case 'O': return 0x644331;
    case 'b': return 0xCB934F;
    case 'G': return 0x79AC69;
    case 'R': return 0xBD6655;
    case 'P': return 0xB5A4E7;
    case 'C': return 0x7DADCB;
    case 'D': return 0x51637F;
    default: return 0;
    }
}

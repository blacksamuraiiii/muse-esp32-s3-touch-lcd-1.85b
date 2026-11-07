/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_food_art.h -- 1.5a feed-subpage food pixel art.
 *
 * Verbatim 16x16 grids, copied from TamaMe
 * (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet_art.c, Apache-2.0;
 * TamaMe attribution retained in muse_pet_model.h and in the design doc).
 * Upstream icon indices: 3 = coffee, 4 = cake, 5 = rice, 8 = tea.
 * Six foods since 2026-11-07 (user decision): the 1.5a port kept
 * only rice/cake/coffee and dropped hotpot/chicken/tea (chicken reads as a
 * brown blob at 80px, tea is another cup like coffee, hotpot is too dark);
 * the user asked the three back, then swapped chicken for pizza, tea
 * for milk tea, hotpot for skewers and skewers for a burger (the hotpot
 * pot reads as a brown blob, and the skewers never looked right).
 * Pizza and the burger are NEW, drawn here in the same 16x16 grid
 * and colour map (they are NOT upstream). Milk tea IS upstream icon 8
 * (a cup with a straw and boba pearls), renamed from 茶 to 奶茶.
 * Chicken (icon 7) and hotpot (icon 6) stay out.
 *
 * Colour map is upstream muse_pet_art_pixel()'s switch, verbatim:
 *  y = 0xA76A19, Y = 0xF6CB4B, W = 0xFFF2CE, O = 0x644331,
 *  b = 0xCB934F, G = 0x79AC69, R = 0xBD6655, P = 0xB5A4E7,
 *  C = 0x7DADCB, D = 0x51637F; space = transparent (do not draw).
 * The grids use W, O, R, b, G, Y -- the full map is kept so a
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

/* Order is the feed-subpage strip order: rice, cake, coffee, burger,
 * pizza, milk tea. Must match muse_pet_feed_index() (0..5). */
static const muse_interact_food_art_t MUSE_INTERACT_FOODS[6] = {
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
    {
        /* Hamburger: NEW, drawn here in the same 16x16 grid and colour
         * map (not upstream) on 2026-11-07 (user). Top bun,
         * lettuce, cheese, tomato, patty, bottom bun. Sesame:
         * three white dots scattered at random (user: not
         * symmetric). Replaces the skewer (Mard 883). */
        "汉堡包",
        {
            "                ",
            "                ",
            "      bbbb      ",
            "    bbbbbWbb    ",
            "   bbWbbbbbbb   ",
            "   bbbbbbbbWb   ",
            "  bbbbbbbbbbbb  ",
            " GGGGGGGGGGGGGG ",
            " YYYYYYYYYYYYYY ",
            "  RRRRRRRRRRRR  ",
            "  OOOOOOOOOOOO  ",
            "  OOOOOOOOOOOO  ",
            "  bbbbbbbbbbbb  ",
            "   bbbbbbbbbb   ",
            "                ",
            "                ",
        },
    },
    {
        /* Pizza slice: NEW, the very first design drawn for the 6-food
         * set (wide brown crust, yellow cheese, red pepperoni
         * clusters, green pepper near the tip), redrawn at the
         * angle the user specified on 2026-11-07: tip at the
         * bottom-left, crust at the top-right. (A 45-deg raster
         * rotation of the original was too messy, so this is a
         * clean hand redraw at that angle; the Mard(707)
         * version and the triangle reshape are both dropped.) */
        "披萨",
        {
            "                ",
            "                ",
            "       bbb      ",
            "      Ybbbb     ",
            "      YYYYbb    ",
            "     YYYYYYbb   ",
            "     YYRRYYYbb  ",
            "     YYYYYYYbb  ",
            "    YYYYYYYbb   ",
            "    YYYRRYYY    ",
            "   YYYYYYY      ",
            "   YYGYYY       ",
            "   YYYY         ",
            "   YY           ",
            "   Y            ",
            "                ",
        },
    },
    {
        /* Milk tea: upstream icon 8 (tea), verbatim, renamed from 茶.
         * A cup with a straw and boba pearls -- it already reads as
         * bubble / milk tea. */
        "奶茶",
        {
            "       RR       ",
            "       RR       ",
            "       RR       ",
            " WWWWWWWWWWWW   ",
            "  WWWWWWWWWW    ",
            "  bbbbbbbbbb    ",
            "  bbbbbbbbbb    ",
            "   bbbbbbbb     ",
            "   ObbObbOb     ",
            "   bbObbObb     ",
            "   ObbObbOb     ",
            "    bbbbbb      ",
            "    ObObOb      ",
            "    bbbbbb      ",
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

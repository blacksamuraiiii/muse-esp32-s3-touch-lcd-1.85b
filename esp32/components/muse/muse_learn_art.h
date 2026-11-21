/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_learn_art.h -- 学习子页 four-card icon masters (mockup v6
 * + 语文练习 2026-11-20).
 *
 * Source (user decision 2026-11-12, mockup v6, same set as the word
 * cards; 语文 icon user decision 2026-11-20, mockup v1):
 *   game-icons/icons repository, CC BY 3.0 -- keep the Game-icons
 *   attribution in the release notes.
 *     pencil_brush = delapouite/pencil-brush.svg (Delapouite, 语文练习)
 *     calculator = delapouite/calculator.svg  (Delapouite, 数学练习)
 *     open_book  = lorc/open-book.svg         (Lorc, 英语单词)
 *     headphones = delapouite/headphones.svg  (Delapouite, 英语听力;
 *                  replaced the microphone 2026-11-21 when the row
 *                  was renamed from 英语口语)
 *   All four are 512x512 single-colour SVGs. Each card -- and the
 *   main grid since 2026-11-20 (MUSE_MAIN_GRID_ART_COLOR) -- is
 *   recoloured at runtime with its own colour, the word-cards
 *   idiom (MUSE_LEARN_ART_COLOR below): pencil_brush vermilion
 *   #ff7a59, calculator gold #f6cb4b (= MUSE_THEME_GOLD), open_book
 *   light blue #8ec9ff, microphone pink #ff8fb3 (user-confirmed,
 *   mockup v6; 语文 colour mockup v1).
 *
 * Generated once, offline, by ~/workspace/learn-cards-art-gen.py
 * (pencil_brush by ~/workspace/learn-pencil-brush-gen.py); the
 * four .c files beside this header hold the pixel data (LVGL A8,
 * 128x128 master). Do not hand-edit them and do not hand-draw
 * replacements: the only approved art is the set above (the v7
 * hand-drawn 16x16 pixel-art proposal was shown to the user and
 * turned down 2026-11-12 in favour of this v6 single-colour set).
 *
 * Format is A8, not ARGB8888: one ARGB8888 128x128 master is 64 KB,
 * and four icons would be 256 KB against the ~50 KB free in the
 * 4 MB app slot at 1.5.4b. A8 is 16 KB per icon (64 KB for the
 * four). The UI recolours the mask per card at runtime
 * (lv_obj_set_style_image_recolor + _opa COVER), exactly like the
 * word-cards art.
 *
 * Regenerate:
 *   1. take each SVG, delete its first full-canvas path
 *      (<path d="M0 0h512v512H0z"/>), which would render as a black square;
 *   2. render 256x256 PNG, white on transparent, then Lanczos-resize
 *      to 128x128 (one master, LVGL scales it at runtime);
 *   3. python3 ~/workspace/learn-cards-art-gen.py
 *      (writes the three .c files; this header's list is by hand).
 *   The pencil_brush master is generated the same way by
 *   ~/workspace/learn-pencil-brush-gen.py (its IoU check is the same
 *   bar: >= 0.99 against the white 240 px render).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_learn_art_pencil_brush;
extern const lv_image_dsc_t muse_learn_art_calculator;
extern const lv_image_dsc_t muse_learn_art_open_book;
extern const lv_image_dsc_t muse_learn_art_headphones;

/* Master size, px (square). The UI draws the picture at 80 px. */
#define MUSE_LEARN_ART_PX 128

/* Card slot -> descriptor, in learn-page order
 * (语文练习, 数学练习, 英语单词, 英语听力). */
enum {
    MUSE_LEARN_TRACING = 0,
    MUSE_LEARN_MATH,
    MUSE_LEARN_WORDS,
    MUSE_LEARN_QUIZ,
    MUSE_LEARN_COUNT,
};

extern const lv_image_dsc_t *const MUSE_LEARN_ART[MUSE_LEARN_COUNT];

/* Recolour per card (theme hex), in the same order. */
extern const uint32_t MUSE_LEARN_ART_COLOR[MUSE_LEARN_COUNT];

#ifdef __cplusplus
}
#endif

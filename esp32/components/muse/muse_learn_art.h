/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_learn_art.h -- 学习子页 four-card icon masters (mockup v6
 * + 语文练习 2026-11-20 + 数学/单词换图 2026-11-22).
 *
 * Source (user decision 2026-11-12, mockup v6, same set as the word
 * cards; 语文 icon user decision 2026-11-20, mockup v1; 数学/单词
 * icons user decision 2026-11-22, desktop mockups
 * 学习页卡片候选-20261122.png / 学习页卡片定稿-20261122.png):
 *   game-icons/icons repository, CC BY 3.0 -- keep the Game-icons
 *   attribution in the release notes.
 *     pencil_brush = delapouite/pencil-brush.svg (Delapouite, 语文练习)
 *     abacus       = a Chinese abacus (suanpan) drawn to the user's
 *                    spec 2026-11-22 -- elongated outer frame with
 *                    clear margins, seven rods, uniform beads all
 *                    resting down (the upper pair on the beam, the
 *                    lower five on the bottom rail) (数学练习;
 *                    replaces the calculator, which read as an
 *                    office tool). game-icons has no Chinese abacus
 *                    (its delapouite/abacus is a bead-on-wire
 *                    counting frame with no beam), so this one
 *                    master is drawn programmatically in the same
 *                    flat style, by user direction -- the only
 *                    sanctioned exception to the game-icons set.
 *     apple        = the word-cards' own muse_wordcards_art_apple
 *                    master (英语单词): the feature shows picture
 *                    cards, so the card shows one (replaces
 *                    lorc/open-book, which read as reading). The
 *                    descriptor lives in muse_wordcards_art.h; the
 *                    learn table points at it, so no new art file.
 *     headphones = delapouite/headphones.svg  (Delapouite, 英语听力;
 *                  replaced the microphone 2026-11-21 when the row
 *                  was renamed from 英语口语)
 *   Each card -- and the main grid since 2026-11-20
 *   (MUSE_MAIN_GRID_ART_COLOR) -- is recoloured at runtime with its
 *   own colour, the word-cards idiom (MUSE_LEARN_ART_COLOR below):
 *   pencil_brush bamboo green #7bcf9e (moved off vermilion
 *   2026-11-22 so the red apple owns the warm slot), abacus gold
 *   #f6cb4b (= MUSE_THEME_GOLD), apple red #ff4136 (an apple is
 *   red; user 2026-11-22), headphones pink #ff8fb3.
 *
 * Generated once, offline, by ~/workspace/learn-cards-art-gen.py
 * (pencil_brush by ~/workspace/learn-pencil-brush-gen.py); the
 * three learn-owned masters live in .c files beside this header
 * (LVGL A8, 128x128 master). Do not hand-edit them and do not
 * hand-draw further replacements: the v7 hand-drawn 16x16
 * pixel-art proposal was shown to the user and turned down
 * 2026-11-12, and the 2026-11-22 abacus above is the only
 * user-directed drawing since.
 *
 * Format is A8, not ARGB8888: one ARGB8888 128x128 master is 64 KB,
 * and four icons would be 256 KB against the ~50 KB free in the
 * 4 MB app slot at 1.5.4b. A8 is 16 KB per icon (64 KB for the
 * four; the words card reuses an already-linked master, so the
 * 2026-11-22 swap adds only the abacus). The UI recolours the mask
 * per card at runtime (lv_obj_set_style_image_recolor + _opa
 * COVER), exactly like the word-cards art.
 *
 * Regenerate (game-icons entries):
 *   1. take each SVG, delete its first full-canvas path
 *      (<path d="M0 0h512v512H0z"/>), which would render as a black square;
 *   2. render 256x256 PNG, white on transparent, then Lanczos-resize
 *      to 128x128 (one master, LVGL scales it at runtime).
 * The abacus master is drawn (frame, beam, rods, bead pills) on a
 * 512-square canvas, then Lanczos-resized the same way. (C-1,
 * 2026-11-23: the .c header said 512 and this file said 1024; the
 * 512 canvas fits the committed master clearly better -- mean
 * |alpha| diff 5.32 vs 6.86 at 1024, mask IoU 0.9654 vs 0.9584 --
 * so 512 is the real canvas. Generator:
 * tools/muse/gen_learn_abacus_art.py; the regenerated master is a
 * documented reproduction, not byte-identical -- see there for the
 * geometry and the sub-pixel gap against the committed file.)
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_learn_art_pencil_brush;
extern const lv_image_dsc_t muse_learn_art_abacus;
extern const lv_image_dsc_t muse_learn_art_headphones;
/* 英语单词 uses the word-cards apple: muse_wordcards_art_apple,
 * declared in muse_wordcards_art.h. */

/* Master size, px (square). The 2x2 grid draws the picture at
 * 56 px (IC_GRID_ICON in muse_interact_ui.c; the 80 px note was
 * the old three-card row's size). */
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

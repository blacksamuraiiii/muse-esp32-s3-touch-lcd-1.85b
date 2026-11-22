/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_learn_art.c -- art tables (no pixels: the three learn-owned
 * masters live in muse_learn_art_{pencil_brush,abacus,headphones}.c;
 * the words card reuses the word-cards apple master declared in
 * muse_wordcards_art.h). Do not hand-edit the order -- it must
 * match the learn-page card order in muse_interact_ui.c
 * (语文练习, 数学练习, 英语单词, 英语听力).
 */
#include "muse_learn_art.h"
#include "muse_wordcards_art.h"

const lv_image_dsc_t *const MUSE_LEARN_ART[MUSE_LEARN_COUNT] = {
    &muse_learn_art_pencil_brush,
    &muse_learn_art_abacus,
    &muse_wordcards_art_apple,
    &muse_learn_art_headphones,
};

const uint32_t MUSE_LEARN_ART_COLOR[MUSE_LEARN_COUNT] = {
    0x7bcf9e, /* pencil_brush (bamboo green since 2026-11-22: gave up
                 the vermilion so the red apple owns the warm slot;
                 vermilion had been the colour since mockup v1) */
    0xf6cb4b, /* abacus (gold, = MUSE_THEME_GOLD, mockup v6) */
    0xff4136, /* apple (red -- an apple is red, user 2026-11-22; the
                 slot was open_book light blue #8ec9ff, mockup v6) */
    0xff8fb3, /* headphones (pink, mockup v6; was the microphone until
                 英语口语 became 英语听力, 2026-11-21) */
};

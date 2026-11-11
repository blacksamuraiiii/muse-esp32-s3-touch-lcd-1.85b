/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards_art.h -- 英语单词卡片 art masters (phase 1 + 2).
 *
 * Source (user decision 2026-11-10, same set as 石头剪刀布):
 *   game-icons/icons repository, CC BY 3.0 -- keep the Game-icons
 *   attribution in the release notes.
 *     apple  = lorc/shiny-apple.svg (Lorc)
 *     banana = delapouite/banana.svg (Delapouite)
 *     cherry = delapouite/cherry.svg (Delapouite)
 *     peach  = delapouite/peach.svg (Delapouite)
 *     cat    = lorc/cat.svg (Lorc)
 *     dog    = delapouite/sitting-dog.svg (Delapouite, no plain dog)
 *     bird   = delapouite/hummingbird.svg (Delapouite, no plain bird)
 *     fish   = delapouite/tropical-fish.svg (Delapouite)
 *     bear   = delapouite/bear-head.svg (Delapouite, head only)
 *     duck   = delapouite/duck.svg (Delapouite)
 *     frog   = lorc/frog.svg (Lorc)
 *     fox    = lorc/fox-head.svg (Lorc, head only)
 *     balloon = delapouite/air-balloon.svg (Delapouite)
 *     box    = delapouite/cardboard-box.svg (Delapouite)
 *     bag    = delapouite/hand-bag.svg (Delapouite)
 *     robot  = lorc/robot-golem.svg (Lorc)
 *   All sixteen are 512x512 single-colour SVGs.
 *
 * Generated once, offline, by ~/workspace/wordcards-art-gen-phase2.py;
 * the sixteen .c files beside this header hold the pixel data
 * (LVGL A8, 128x128 master). Do not hand-edit them and do not
 * hand-draw replacements: the only approved art is the set above.
 *
 * Format is A8, not the hands' ARGB8888: one ARGB8888 128x128 master
 * is 64 KB, and sixteen icons would be 1 MB against the ~290 KB free
 * in the 4 MB app slot at 1.5.2a (1.5e's bin was 3,870,720 B). A8 is
 * 16 KB per icon (256 KB for the four sets). The UI recolours the mask
 * per word at runtime (lv_obj_set_style_image_recolor + _opa COVER),
 * exactly like the hands' loser-dim -- the colour lives in the
 * word table (muse_wordcards.h), not in these pixels.
 *
 * Regenerate:
 *   1. take each SVG, delete its first full-canvas path
 *      (<path d="M0 0h512v512H0z"/>), which would render as a black square;
 *   2. render 256x256 PNG, white on transparent, then Lanczos-resize
 *      to 128x128 (one master, LVGL scales it at runtime);
 *   3. python3 ~/workspace/wordcards-art-gen-phase2.py
 *      (writes the sixteen .c files + this header's list is by hand).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_wordcards_art_apple;
extern const lv_image_dsc_t muse_wordcards_art_banana;
extern const lv_image_dsc_t muse_wordcards_art_cherry;
extern const lv_image_dsc_t muse_wordcards_art_peach;
extern const lv_image_dsc_t muse_wordcards_art_cat;
extern const lv_image_dsc_t muse_wordcards_art_dog;
extern const lv_image_dsc_t muse_wordcards_art_bird;
extern const lv_image_dsc_t muse_wordcards_art_fish;
extern const lv_image_dsc_t muse_wordcards_art_bear;
extern const lv_image_dsc_t muse_wordcards_art_duck;
extern const lv_image_dsc_t muse_wordcards_art_frog;
extern const lv_image_dsc_t muse_wordcards_art_fox;
extern const lv_image_dsc_t muse_wordcards_art_balloon;
extern const lv_image_dsc_t muse_wordcards_art_box;
extern const lv_image_dsc_t muse_wordcards_art_bag;
extern const lv_image_dsc_t muse_wordcards_art_robot;

/* Master size, px (square). The UI draws the picture at 72 px. */
#define MUSE_WORDCARDS_ART_PX 128

/* Art index -> descriptor, in word-table order (muse_wordcards.h). */
extern const lv_image_dsc_t *const MUSE_WORDCARDS_ART[16];

/* Recolour per word (theme hex), in the same order. */
extern const uint32_t MUSE_WORDCARDS_ART_COLOR[16];

#ifdef __cplusplus
}
#endif

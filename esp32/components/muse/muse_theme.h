/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_theme.h -- the one place the UI colours live (1.5b).
 *
 * muse_ui.c, muse_interact_ui.c, muse_settings_ui.c, muse_menu.c and
 * muse_keypad.c used to define the same values under COLOR_* / IC_* in
 * five places, so a theme change needed five edits and always missed one.
 * The values here are unchanged from those copies; only the definitions
 * moved. Files that need a colour include this header and use MUSE_THEME_*.
 */
#pragma once

#define MUSE_THEME_TEXT        0xf2efff
#define MUSE_THEME_DIM         0x8b84a8
#define MUSE_THEME_BG          0x000000
#define MUSE_THEME_CARD        0x1a1530
#define MUSE_THEME_CARD_PRESSED 0x2e2552
#define MUSE_THEME_ACCENT      0xa77dff
#define MUSE_THEME_OK          0x6ff0bf
#define MUSE_THEME_WARN        0xffb45c
#define MUSE_THEME_DANGER      0xff5c5c
#define MUSE_THEME_GOLD        0xf6cb4b
/* The speaker hint's lit and unlit golds (muse_ui.c). */
#define MUSE_THEME_GOLD_HI     0xffd060
#define MUSE_THEME_GOLD_LO     0x8b7845
/* The microphone hint's listening / idle blues (muse_ui.c): the one cool
 * hue in the theme, kept to mark "Muse is listening" apart from ACCENT. */
#define MUSE_THEME_MIC_ACTIVE  0x64d4ff
#define MUSE_THEME_MIC_IDLE    0x80bfff
/* muse_ui.c only, kept here so no colour is defined anywhere else. */
#define MUSE_THEME_CAPTION     0xd8d2ff
#define MUSE_THEME_RING_BG     0x140f22
#define MUSE_THEME_METER_OFF   0x1d1733
#define MUSE_THEME_DOT_OFF     0x3a3358
#define MUSE_THEME_SELECTED    0x2e2552
#define MUSE_THEME_RULE        0x2a2345

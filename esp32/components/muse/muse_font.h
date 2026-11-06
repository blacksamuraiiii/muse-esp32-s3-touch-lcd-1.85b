/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_font.h -- the one shared font chain (1.5b, Noto since 2026-11-03,
 * unified to Noto 16 on 2026-11-06).
 *
 * muse_ui.c and muse_interact_ui.c used to implement the same fallback
 * chain twice each, and the 1.5a reply fix (f0af50b) changed one copy but
 * not the other, which is how replies kept the old 20 px face. There is
 * now exactly one chain, and every page uses it:
 *
 *   muse_font_cjk()     -- 16 px Noto Sans SC Medium at 4bpp (upstream's
 *                          GB2312 face, 6763 hanzi + punctuation, kana and
 *                          fullwidth forms, plus ASCII + Latin-1 since the
 *                          2026-11-06 unification; its own 31/9 line
 *                          metrics are pinned to the 20/5 line box in the
 *                          .c file), falling back to the DejaVu symbol
 *                          face below.
 *   muse_font_caption() -- unscii-16, falling back to muse_font_cjk().
 *                          This is what captions AND replies use since
 *                          1.5b; there is no separate 20 px reply face.
 *   muse_font_reply()   -- the same Noto 16 face as muse_font_cjk().
 *                          (Before the unification the ASCII here landed
 *                          on Montserrat 14, which read a size smaller
 *                          than the hanzi.)
 *   muse_font_btn()     -- the same Noto 16 face as muse_font_cjk().
 *                          (Before the unification buttons were
 *                          Montserrat 20 with the CJK fallback, so
 *                          English on a button read a size larger than
 *                          the hanzi.)
 *
 * Montserrat 16/14/20/28 and unscii-16/8 still come from LVGL itself
 * and stay in the build: the top-bar clock and battery, the keypad, the
 * pairing code and the LV_SYMBOL_* icons take bare Montserrat directly.
 * Only the shared text chain is unified; nothing else moves. (Measured
 * 2026-11-06: the unification adds ~28 KB to the Noto face and removes
 * no Montserrat size, so it is for consistency, not flash.)
 *
 * Without CONFIG_MUSE_CJK_FONT the chains collapse to the Latin faces.
 */
#pragma once

#include "lvgl.h"

const lv_font_t *muse_font_cjk(void);
const lv_font_t *muse_font_caption(void);
const lv_font_t *muse_font_reply(void);
const lv_font_t *muse_font_btn(void);

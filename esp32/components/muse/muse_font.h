/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_font.h -- the one shared CJK font chain (1.5b, Noto since 2026-11-03).
 *
 * muse_ui.c and muse_interact_ui.c used to implement the same fallback
 * chain twice each, and the 1.5a reply fix (f0af50b) changed one copy but
 * not the other, which is how replies kept the old 20 px face. There is
 * now exactly one chain, and every page uses it:
 *
 *   muse_font_cjk()     -- 16 px Noto Sans SC Medium at 4bpp (upstream's
 *                          GB2312 face, 6763 hanzi + punctuation, kana and
 *                          fullwidth forms; its own 31/9 line metrics are
 *                          pinned to the 20/5 line box in the .c file),
 *                          falling back to Montserrat 16, then to the
 *                          DejaVu symbol face below.
 *   muse_font_mont16_cjk() -- Montserrat 16 look with the full chain
 *                          behind it. For labels whose text is mostly
 *                          Latin but can carry user text (a Wi-Fi SSID, a
 *                          row value, a note): bare Montserrat 16 has no
 *                          CJK glyph, which drew a Chinese SSID as tofu
 *                          boxes on the Wi-Fi page and the SETTINGS row
 *                          (user photo, 2026-11-03). Never use bare
 *                          Montserrat 16 for a label that can hold a name.
 *   muse_font_caption() -- unscii-16, falling back to muse_font_cjk().
 *                          This is what captions AND replies use since
 *                          1.5b; there is no separate 20 px reply face.
 *   muse_font_reply()   -- Noto 16 leading, Montserrat 14 for ASCII (it
 *                          reads at the same size as a 16 px hanzi), then
 *                          the symbol face.
 *   muse_font_btn()     -- Montserrat 20, falling back to muse_font_cjk().
 *
 * Without CONFIG_MUSE_CJK_FONT the chains collapse to the Latin faces.
 */
#pragma once

#include "lvgl.h"

const lv_font_t *muse_font_cjk(void);
const lv_font_t *muse_font_mont16_cjk(void);
const lv_font_t *muse_font_caption(void);
const lv_font_t *muse_font_reply(void);
const lv_font_t *muse_font_btn(void);

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_font.h -- the one shared CJK font chain (1.5b).
 *
 * muse_ui.c and muse_interact_ui.c used to implement the same fallback
 * chain twice each, and the 1.5a reply fix (f0af50b) changed one copy but
 * not the other, which is how replies kept the old 20 px face. There is
 * now exactly one chain, and every page uses it:
 *
 *   muse_font_cjk()     -- 16 px WenQuanYi Micro Hei at 4bpp (the level-1
 *                          face from tools/muse/gen_ui_font.sh), falling
 *                          back to Unifont 16 px 1bpp, then Montserrat 16.
 *   muse_font_caption() -- unscii-16, falling back to muse_font_cjk().
 *                          This is what captions AND replies use since
 *                          1.5b; there is no separate 20 px reply face.
 *   muse_font_btn()     -- Montserrat 20, falling back to muse_font_cjk().
 *
 * Without CONFIG_MUSE_CJK_FONT the chains collapse to the Latin faces.
 */
#pragma once

#include "lvgl.h"

const lv_font_t *muse_font_cjk(void);
const lv_font_t *muse_font_caption(void);
const lv_font_t *muse_font_btn(void);

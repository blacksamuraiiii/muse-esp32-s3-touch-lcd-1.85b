/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_font.c -- the one shared CJK font chain, see muse_font.h.
 *
 * LVGL fallback fonts are per-font pointers, so each chain link is a
 * static lv_font_t copied from the generated face with .fallback set to
 * the next link. The copies are built lazily on first use, in the LVGL
 * task, exactly as the old per-file helpers did.
 */
#include "muse_font.h"

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
LV_FONT_DECLARE(muse_font_ui_16)

/* Long-tail CJK, and the ASCII the CJK faces lack: Unifont's 16x16 bitmap
 * starts at U+3000, so every space, digit and '+' in a Chinese line used
 * to draw as a tofu box (user photo, 1.5a). Montserrat fills exactly those. */
static const lv_font_t *cjk_fallback_face(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = muse_font_cjk_16;
        font.fallback = &lv_font_montserrat_16;
    }
    return &font;
}

const lv_font_t *muse_font_cjk(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = muse_font_ui_16;
        font.fallback = cjk_fallback_face();
    }
    return &font;
}

const lv_font_t *muse_font_caption(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_unscii_16;
        font.fallback = muse_font_cjk();
    }
    return &font;
}

/*
 * 1.5b: what a reply and the caption band under Muse draw with. The
 * antialiased 16 px face leads, so the hanzi are smooth; ASCII has no glyph in
 * either CJK face, so it lands on Montserrat 14, which reads at the same size
 * as a 16 px hanzi. The status words, the name row and the hints keep
 * muse_font_caption() -- the pixel look they had.
 */
const lv_font_t *muse_font_reply(void)
{
    static lv_font_t ascii, font;
    if (!font.get_glyph_dsc) {
        ascii = muse_font_cjk_16;
        ascii.fallback = &lv_font_montserrat_14;
        font = muse_font_ui_16;
        font.fallback = &ascii;
    }
    return &font;
}

const lv_font_t *muse_font_btn(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_montserrat_20;
        font.fallback = muse_font_cjk();
    }
    return &font;
}

#else /* !CONFIG_MUSE_CJK_FONT */

const lv_font_t *muse_font_cjk(void) { return &lv_font_montserrat_16; }
const lv_font_t *muse_font_caption(void) { return &lv_font_unscii_16; }
const lv_font_t *muse_font_reply(void) { return &lv_font_unscii_16; }
const lv_font_t *muse_font_btn(void) { return &lv_font_montserrat_20; }

#endif

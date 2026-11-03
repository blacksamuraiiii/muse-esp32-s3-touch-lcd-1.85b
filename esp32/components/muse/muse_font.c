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
LV_FONT_DECLARE(muse_font_cjk_noto_16)
LV_FONT_DECLARE(muse_font_sym_16)

/* Last link: the symbol tail neither Noto nor Montserrat has. Noto Sans SC
 * covers CJK + fullwidth only, Montserrat 16 covers ASCII + the private-use
 * LVGL icons, and the Latin-1 symbols, general punctuation, degree-C/F,
 * arrows and geometric shapes the UI prints (weather 25℃, hints, reply
 * text) live in the DejaVu cut in fonts/muse_font_sym_16.c. */
static const lv_font_t *sym_face(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = muse_font_sym_16;
        font.fallback = NULL;
    }
    return &font;
}

/* The ASCII Montserrat 16 ends on the symbol face. Every 16 px label that
 * used to take bare &lv_font_montserrat_16 should take this or
 * muse_font_mont16_cjk() instead: bare Montserrat has no CJK glyph, which
 * is how a Chinese SSID drew as tofu boxes on the Wi-Fi page and on the
 * SETTINGS row (user photo, 2026-11-03). */
static const lv_font_t *latin_face(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_montserrat_16;
        font.fallback = sym_face();
    }
    return &font;
}

const lv_font_t *muse_font_cjk(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = muse_font_cjk_noto_16;
        font.fallback = latin_face();
    }
    return &font;
}

/* Montserrat 16 look, full chain behind it: for labels whose text is
 * mostly Latin but can carry user text (a Wi-Fi SSID, a row value, a
 * note). Latin renders pixel-identical to bare Montserrat 16; Chinese
 * and symbols fall through to Noto and the symbol face instead of tofu. */
const lv_font_t *muse_font_mont16_cjk(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_montserrat_16;
        font.fallback = muse_font_cjk();
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
 * What a reply and the caption band under Muse draw with. The
 * antialiased 16 px face leads, so the hanzi are smooth; ASCII has no
 * glyph in the Noto face, so it lands on Montserrat 14, which reads at
 * the same size as a 16 px hanzi, and symbols land on the symbol face.
 * The status words, the name row and the hints keep
 * muse_font_caption() -- the pixel look they had.
 */
const lv_font_t *muse_font_reply(void)
{
    static lv_font_t ascii_face, font;
    if (!font.get_glyph_dsc) {
        ascii_face = lv_font_montserrat_14;
        ascii_face.fallback = sym_face();
        font = muse_font_cjk_noto_16;
        font.fallback = &ascii_face;
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
const lv_font_t *muse_font_mont16_cjk(void) { return &lv_font_montserrat_16; }
const lv_font_t *muse_font_caption(void) { return &lv_font_unscii_16; }
const lv_font_t *muse_font_reply(void) { return &lv_font_unscii_16; }
const lv_font_t *muse_font_btn(void) { return &lv_font_montserrat_20; }

#endif

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_font.c -- the one shared font chain, see muse_font.h.
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

/* Last link: the symbol tail the Noto face lacks. Noto covers CJK +
 * fullwidth + ASCII + Latin-1 (since the 2026-11-06 unification), so
 * the tail only carries what Noto has not: the Latin-1 symbols,
 * general punctuation, degree-C/F, arrows and geometric shapes the UI
 * prints (weather 25℃, hints, reply text), cut from DejaVu in
 * fonts/muse_font_sym_16.c. */
/* Before the tail ends: the LVGL private-use icons (LV_SYMBOL_WIFI,
 * LV_SYMBOL_POWER, ...). Neither Noto nor the DejaVu cut carries U+E000..
 * U+F8FF, so after the unification muse_font_btn() had no source for the
 * icons the settings rows and menu hints draw -- they vanished. Montserrat
 * 16 is the one built-in face that has them, and 16 px matches the text
 * size the chain now uses for buttons. */
static const lv_font_t *icon_face(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = lv_font_montserrat_16;
        font.fallback = NULL;
    }
    return &font;
}

static const lv_font_t *sym_face(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = muse_font_sym_16;
        font.fallback = icon_face();
    }
    return &font;
}

/* The one 16 px face for all text (2026-11-06 unification): Noto Sans SC
 * Medium, now with ASCII + Latin-1 in the face, falling back to the
 * symbol tail only. English renders in Noto at the same 16 px as the
 * hanzi -- no more Montserrat 16/14/20 mixed into labels, buttons and
 * replies. Montserrat itself stays in the build for the clock, the
 * keypad, the pairing code and the LV_SYMBOL_* icons, which still take
 * bare Montserrat directly (see fonts/README.md). */
const lv_font_t *muse_font_cjk(void)
{
    static lv_font_t font;
    if (!font.get_glyph_dsc) {
        font = muse_font_cjk_noto_16;
        font.fallback = sym_face();
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
 * What a reply draws with: the same Noto 16 face as everything else,
 * symbols on the symbol face. (Before the unification the ASCII here
 * landed on Montserrat 14, which read a size smaller than the hanzi.)
 * The status words, the name row and the hints keep
 * muse_font_caption() -- the pixel look they had.
 */
const lv_font_t *muse_font_reply(void)
{
    return muse_font_cjk();
}

const lv_font_t *muse_font_btn(void)
{
    return muse_font_cjk();
}

#else /* !CONFIG_MUSE_CJK_FONT */

const lv_font_t *muse_font_cjk(void) { return &lv_font_montserrat_16; }
const lv_font_t *muse_font_caption(void) { return &lv_font_unscii_16; }
const lv_font_t *muse_font_reply(void) { return &lv_font_unscii_16; }
const lv_font_t *muse_font_btn(void) { return &lv_font_montserrat_20; }

#endif

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_widgets.c -- the one definition of the shared subpage pieces
 * (see muse_widgets.h).
 */

#include "muse_widgets.h"

#include "muse_font.h"
#include "muse_theme.h"

lv_obj_t *muse_back_button(lv_obj_t *parent, lv_event_cb_t back_cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 56, 48);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -112, 28);
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(b);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(MUSE_THEME_ACCENT), 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    return b;
}

lv_obj_t *muse_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, int radius)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_THEME_CARD_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, radius, 0);
    /* The default theme paints lv_btn with a shadow, a border and
     * pad_hor 16 / pad_ver 10 (LV_DPI_DEF 130, DISP_MEDIUM): the five
     * copies this replaces each fought a different subset of those, which
     * is how their looks had drifted. One clean slate here instead. */
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

lv_obj_t *muse_page_title(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_obj_set_style_text_font(t, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(MUSE_THEME_ACCENT), 0);
    lv_label_set_text(t, text);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, y);
    return t;
}

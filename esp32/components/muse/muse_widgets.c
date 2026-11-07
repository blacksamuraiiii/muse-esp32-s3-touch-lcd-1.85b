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

/* --- floating score label ("+1" / "-5") ---------------------------- */
/* Rise and fade share one animation: the exec callback sets y and derives
 * opacity from how far the label has risen, so nothing is still running
 * when the completed callback deletes the label. The start y travels in
 * the label's user data. */
#define MUSE_FLOAT_TEXT_RISE_PX 36
#define MUSE_FLOAT_TEXT_MS 700

static void float_text_anim_cb(void *var, int32_t y)
{
    lv_obj_t *lbl = (lv_obj_t *)var;
    int y0 = (int)(intptr_t)lv_obj_get_user_data(lbl);
    lv_obj_set_y(lbl, (lv_coord_t)y);
    int progress = y0 - y; /* 0..MUSE_FLOAT_TEXT_RISE_PX */
    int opa = 255 - (progress * 255) / MUSE_FLOAT_TEXT_RISE_PX;
    if (opa < 0) {
        opa = 0;
    }
    lv_obj_set_style_opa(lbl, (lv_opa_t)opa, LV_PART_MAIN);
}

static void float_text_anim_done(lv_anim_t *a)
{
    /* LVGL 9.5 has no lv_anim_get_var(): the completed callback runs after
     * the animation was unlinked from the list, so a->var is still valid
     * and deleting the object cannot free this animation under ourselves. */
    lv_obj_delete((lv_obj_t *)a->var);
}

void muse_float_text(lv_obj_t *parent, const char *text, lv_color_t colour,
                     int cx, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, colour, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
    /* The label's width is 0 until it lays out, so measure after a forced
     * layout pass or the text starts at the score point instead of
     * straddling it. */
    lv_obj_update_layout(lbl);
    lv_obj_set_pos(lbl, (lv_coord_t)(cx - lv_obj_get_width(lbl) / 2),
                   (lv_coord_t)y);
    lv_obj_set_user_data(lbl, (void *)(intptr_t)y);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_values(&a, y, y - MUSE_FLOAT_TEXT_RISE_PX);
    lv_anim_set_duration(&a, MUSE_FLOAT_TEXT_MS);
    lv_anim_set_exec_cb(&a, float_text_anim_cb);
    lv_anim_set_completed_cb(&a, float_text_anim_done);
    lv_anim_start(&a);
}

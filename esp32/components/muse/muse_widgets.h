/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_widgets.h -- the shared LVGL building blocks (1.5g).
 *
 * The back arrow was copy-pasted five times (settings, interact, snake,
 * sudoku, whack-a-mole) with the same 56x48 box at TOP_MID(-112, 28) and
 * the same bare ACCENT arrow; the card button existed as five near-copies
 * (settings card, interact main_button/list_row, snake btn, sudoku btn,
 * whack-a-mole round_btn) that had already drifted apart in shadow, border
 * and padding; and every subpage titled itself with its own font and y.
 * These three helpers are now the one definition of each.
 */
#pragma once

#include "lvgl.h"

/* The shared back arrow: 56x48 hit box at TOP_MID(-112, 28), a bare
 * MUSE_THEME_ACCENT LV_SYMBOL_LEFT at Montserrat 20 (no card behind it).
 * Returns the button so a page can hide it (snake hides it while playing);
 * the settings page's compact mode resizes the returned object. */
lv_obj_t *muse_back_button(lv_obj_t *parent, lv_event_cb_t back_cb);

/* The shared card button: CARD background, CARD_PRESSED pressed, no border,
 * no shadow, no padding, not scrollable. w/h accept lv_pct() like
 * lv_obj_set_size does; the caller positions it (set_pos or flex) and adds
 * its own label and event callbacks. Rounded rectangles only -- the
 * whack-a-mole circle keeps its local wrapper and adds its ACCENT ring on
 * top of this. */
lv_obj_t *muse_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, int radius);

/* The shared subpage title: muse_font_cjk() in MUSE_THEME_ACCENT at
 * TOP_MID(0, y). The four game pages and settings all land on this; the
 * menu's list header keeps its own look on purpose (it is a system list,
 * not a subpage). */
lv_obj_t *muse_page_title(lv_obj_t *parent, const char *text, int y);

/* A floating score label ("+1" / "-5"): created under `parent`, centred on
 * cx, rising 36 px while fading out over 700 ms, then deleting itself.
 * One 700 ms lv_anim drives both the rise and the fade (the exec callback
 * derives opacity from the current y), so no second animation is still
 * running when the label goes away.
 *
 * Two game pages spawn these on every score change (金币雨 for +1 / -5,
 * 毛毛跳跃 for +1). The user asked for the same effect on both, so it is
 * one definition here instead of a copy per page.
 *
 * Call it from the LVGL timer that ticks the game, never from a worker. */
void muse_float_text(lv_obj_t *parent, const char *text, lv_color_t colour,
                     int cx, int y);

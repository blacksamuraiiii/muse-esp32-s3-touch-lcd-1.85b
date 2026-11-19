/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_alarm_ui.h"
#include "muse_theme.h"   /* MUSE_THEME_*, the one colour table (1.5b) */
#include "muse_font.h"    /* muse_font_cjk(), the one CJK chain */
#include "muse_widgets.h" /* the shared back arrow / subpage title (1.5g) */

#include <stdio.h>
#include <string.h>

#include "muse_board.h"
#include "muse_input.h"
#include "muse_settings.h"

#include "esp_log.h"

static const char *TAG = "muse_alarm_ui";

/* ------------------------------------------------------------ time picker page */

/* Ring overlay scale (2026-11-19: the user asked for one size step up).
 * The time uses muse_font_clock_48 -- Montserrat Medium 48 px cut down to
 * digits and the colon (~16 KB in flash; the full montserrat_48 face is
 * ~128 KB and does not fit the 4 MB app slot) -- at 1.25x, so ~60 px drawn
 * and crisp, unlike blowing the 28 px bitmap up 2.1x. The 16 px CJK faces
 * (the only CJK size this build carries) scale 1.15x, the most they take
 * before the bitmap edges go soft. Pivots are the box centres, so the drawn
 * glyphs grow symmetrically around the slot they are aligned to. */
#define RING_TIME_SCALE 320   /* 48 px clock face x 1.25 -> ~60 px */
#define RING_CJK_SCALE 294    /* 16 px CJK x 1.15 -> ~18 px */

LV_FONT_DECLARE(muse_font_clock_48);

#define ROLLER_H 126   /* three visible rows */
#define ROLLER_W 110
#define ROLLER_W_SMALL 84

static lv_obj_t *s_hour_roller, *s_min_roller;
static lv_event_cb_t s_back_cb;   /* settings page: navigate back to Alarm */

static void on_page_back(lv_event_t *e)
{
    /* Every exit saves first (the rollers only change the display), then
     * hands navigation back to the settings page. The roller pointers are
     * cleared right after: the settings page drops (deletes) this page on
     * the way out, so a second exit event would otherwise save through a
     * freed roller. The back callback is cleared too, so a second exit
     * event cannot call it again after the page is gone. */
    muse_alarm_ui_time_page_save();
    s_hour_roller = NULL;
    s_min_roller = NULL;
    lv_event_cb_t back_cb = s_back_cb;
    s_back_cb = NULL;
    if (back_cb) {
        back_cb(e);
    }
}

static void on_page_gesture(lv_event_t *e)
{
    (void)e;
    /* A roller eats vertical drags itself; a right swipe is "back". */
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        on_page_back(e);
    }
}

/* "00\n01\n...\nNN" for a roller with `count` options. */
static void roller_options(char *buf, size_t cap, int count)
{
    size_t n = 0;
    for (int i = 0; i < count && n < cap; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "%s%02d", i ? "\n" : "", i);
    }
}

static lv_obj_t *make_roller(lv_obj_t *page, int x, int value, int count)
{
    char opts[60 * 3 + 1];
    roller_options(opts, sizeof(opts), count);
    lv_obj_t *r = lv_roller_create(page);
    lv_obj_set_size(r, muse_board->width < 300 ? ROLLER_W_SMALL : ROLLER_W, ROLLER_H);
    lv_obj_align(r, LV_ALIGN_TOP_MID, x, muse_board->height / 2 - ROLLER_H / 2);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, (uint16_t)value, LV_ANIM_OFF);
    /* Unselected rows are small and dim; the selected row is large, on a
     * card-coloured band (the mockup's slot). */
    lv_obj_set_style_text_font(r, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(MUSE_THEME_DIM), LV_PART_MAIN);
    /* No frame around the wheels (user, 2026-11-18): the theme's default
     * border made each wheel look boxed in; only the selected row band
     * stays visible. */
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(r, 0, LV_PART_SELECTED);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(MUSE_THEME_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(MUSE_THEME_CARD), LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 12, LV_PART_SELECTED);
    /* A drag that runs off the roller's end must not scroll anything
     * behind it (pocket-watch's LV_OBJ_FLAG_SCROLL_CHAIN removal). */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLL_CHAIN);
    return r;
}

lv_obj_t *muse_alarm_ui_build_time_page(lv_obj_t *tile, lv_event_cb_t back_cb)
{
    s_back_cb = back_cb;
    /* Flat short panels, and ones too narrow for the settings list, get
     * the tighter title (same rule as muse_settings_ui.c page()). */
    const bool compact =
        !muse_board->round && (muse_board->height <= 240 || muse_board->width < 330);
    lv_obj_t *p = lv_obj_create(tile);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    /* A swipe bubbles up past the page to the screen by default, so the
     * page has to stop it (same as muse_settings_ui.c catch_swipes). */
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(p, on_page_gesture, LV_EVENT_GESTURE, NULL);

    muse_page_title(p, "Set time", compact ? 12 : 44);
    lv_obj_t *b = muse_back_button(p, on_page_back);
    if (compact) {
        lv_obj_set_size(b, 44, 44);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 4, 0);
    }

    const int roller_w = muse_board->width < 300 ? ROLLER_W_SMALL : ROLLER_W;
    const int dx = roller_w / 2 + 17;   /* room for the ":" between the wheels */
    s_hour_roller = make_roller(p, -dx, muse_settings_alarm_hour(), 24);
    s_min_roller = make_roller(p, dx, muse_settings_alarm_min(), 60);

    lv_obj_t *colon = lv_label_create(p);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(colon, lv_color_hex(MUSE_THEME_TEXT), 0);
    lv_label_set_text(colon, ":");
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, muse_board->height / 2 - 14);

    lv_obj_t *h = lv_label_create(p);
    lv_obj_set_style_text_font(h, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(h, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_label_set_text(h, "时");
    lv_obj_align(h, LV_ALIGN_TOP_MID, -dx, muse_board->height / 2 + ROLLER_H / 2 + 8);
    lv_obj_t *m = lv_label_create(p);
    lv_obj_set_style_text_font(m, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(m, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_label_set_text(m, "分");
    lv_obj_align(m, LV_ALIGN_TOP_MID, dx, muse_board->height / 2 + ROLLER_H / 2 + 8);

    lv_obj_t *hint = lv_label_create(p);
    lv_obj_set_style_text_font(hint, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_label_set_text(hint, "Swipe to pick · saved on exit");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, muse_board->height / 2 + ROLLER_H / 2 + 40);
    return p;
}

void muse_alarm_ui_time_page_save(void)
{
    if (!s_hour_roller || !s_min_roller) {
        return;
    }
    muse_settings_set_alarm_time((int)lv_roller_get_selected(s_hour_roller),
                                 (int)lv_roller_get_selected(s_min_roller));
}

/* ------------------------------------------------------------------ ring page */

static lv_obj_t *s_ring, *s_ring_time, *s_ring_sub;
static bool s_ring_shown;

/* The repeat rule in the user's words, for the "每天 · 闹钟响了" line. */
static const char *ring_days_cn(int days)
{
    switch (days) {
        case 0x00:
            return "一次";
        case 0x7F:
            return "每天";
        case 0x3E:
            return "工作日";
        case 0x41:
            return "周末";
        default:
            return "自定义";
    }
}

/* Does a finger reach the ring overlay at all? Logged so a "the button does
 * nothing" report can be told apart from "the click never arrives". */
static void on_ring_pressed(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    ESP_LOGI(TAG, "ring page: pressed at %d,%d", (int)p.x, (int)p.y);
}

static void on_ring_stop(lv_event_t *e)
{
    (void)e;
    /* The tick hides the page once the ring really ends. */
    const bool stopped = muse_input_alarm_stop();
    ESP_LOGI(TAG, "ring page: stop tapped, stopped=%d", (int)stopped);
}

static void build_ring(void)
{
    const int h = muse_board->height;
    s_ring = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(MUSE_THEME_BG), 0);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
    /* Full-screen and clickable, so taps never fall through to the tile
     * below; no gesture bubble, so a swipe never slides the tileview. */
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ring, on_ring_pressed, LV_EVENT_PRESSED, NULL);

    /* The set time, big: native Montserrat 48 scaled up (RING_TIME_SCALE).
     * Box top h/2-88 puts the drawn glyphs' centre at about h/2-64, one
     * step larger and slightly higher than the 1.5.4 slot so the taller
     * digits keep the same gaps. */
    s_ring_time = lv_label_create(s_ring);
    lv_obj_set_style_text_font(s_ring_time, &muse_font_clock_48, 0);
    lv_obj_set_style_text_color(s_ring_time, lv_color_hex(MUSE_THEME_TEXT), 0);
    lv_obj_set_style_transform_pivot_x(s_ring_time, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_ring_time, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(s_ring_time, RING_TIME_SCALE, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(s_ring_time, RING_TIME_SCALE, LV_PART_MAIN);
    lv_label_set_text(s_ring_time, "07:00");
    lv_obj_align(s_ring_time, LV_ALIGN_TOP_MID, 0, h / 2 - 88);

    s_ring_sub = lv_label_create(s_ring);
    lv_obj_set_style_text_font(s_ring_sub, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(s_ring_sub, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_obj_set_style_transform_pivot_x(s_ring_sub, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_ring_sub, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(s_ring_sub, RING_CJK_SCALE, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(s_ring_sub, RING_CJK_SCALE, LV_PART_MAIN);
    lv_label_set_text(s_ring_sub, "每天 · 闹钟响了");
    lv_obj_align(s_ring_sub, LV_ALIGN_TOP_MID, 0, h / 2 - 12);

    /* One button only (v1: no snooze). DANGER fill, centred; 208x60 with
     * the label at 1.25x, so the tap target grows with the rest of the
     * page instead of staying the 160x48 it was at the smaller size. */
    lv_obj_t *btn = lv_btn_create(s_ring);
    lv_obj_set_size(btn, 208, 60);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, h / 2 + 26);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(MUSE_THEME_DANGER), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_ring_stop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(MUSE_THEME_TEXT), 0);
    lv_obj_set_style_transform_pivot_x(lbl, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(lbl, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(lbl, RING_TIME_SCALE, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(lbl, RING_TIME_SCALE, LV_PART_MAIN);
    lv_label_set_text(lbl, "停止");
    lv_obj_center(lbl);

    lv_obj_t *hint = lv_label_create(s_ring);
    lv_obj_set_style_text_font(hint, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_obj_set_style_transform_pivot_x(hint, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(hint, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(hint, RING_CJK_SCALE, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(hint, RING_CJK_SCALE, LV_PART_MAIN);
    lv_label_set_text(hint, "按 BOOT 也可停止");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, h / 2 + 100);
}

static void refresh_ring(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", muse_settings_alarm_hour(), muse_settings_alarm_min());
    lv_label_set_text(s_ring_time, buf);
    char sub[48];
    snprintf(sub, sizeof(sub), "%s · 闹钟响了", ring_days_cn(muse_settings_alarm_days()));
    lv_label_set_text(s_ring_sub, sub);
}

void muse_alarm_ui_tick(void)
{
    const bool ringing = muse_input_alarm_ringing();
    if (ringing && !s_ring_shown) {
        if (!s_ring) {
            build_ring();
        }
        refresh_ring();
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        s_ring_shown = true;
    } else if (!ringing && s_ring_shown) {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        s_ring_shown = false;
    }
}

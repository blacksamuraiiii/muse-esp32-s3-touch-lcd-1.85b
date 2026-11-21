/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_luckywheel_ui.c -- lucky wheel page (mockup v7).
 *
 * Layout (360x360, snake overlay rules): the 240 px disc centred at
 * (180,180), the gold triangle pointer at the top (166,60) that never
 * moves, the title at y=38 ("幸运转盘", "游戏结束" on the result card),
 * the HUD at y=308, the prompt row at y=304 on the START card. The disc
 * itself is one RGB565 image, rotated with lv_image_set_rotation
 * (0.1-degree units, positive = clockwise, pivot 120,120); the spin is
 * 3 turns, cubic ease-out, ~2.8 s on a 50 ms timer. The disc keeps its
 * landing angle between spins, so the next spin continues from where
 * the last one stopped instead of jumping back to 0.
 *
 * The timer is created once at build and paused when the page is
 * hidden (掷筊 pattern): the same tick advances the spin and hides
 * the limit toast after ~2 s, so no one-shot timer is ever created
 * or deleted mid-flight.
 *
 * Economy (the worker is the authority; the snapshot gates here are
 * advisory only, same as 掷筊): a paid spin costs 2 coins and counts
 * against the 9-a-day ledger; a free re-spin (won by landing on
 * "再来一次") costs nothing and does not count. The free spin is an
 * in-memory flag: it is banked when the AGAIN result lands, consumed
 * by the next spin, and restored if the worker refuses that spin.
 */
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "muse_audio.h"
#include "muse_font.h"
#include "muse_luckywheel.h"
#include "muse_luckywheel_art.h"
#include "muse_luckywheel_ui.h"
#include "muse_pet.h"
#include "muse_pet_model.h"
#include "muse_theme.h"
#include "muse_widgets.h"

/* Geometry, mockup v7 (all px, 360x360 page). */
#define LW_WHEEL_X 60
#define LW_WHEEL_Y 60
#define LW_POINTER_X 166
#define LW_POINTER_Y 60
#define LW_TITLE_Y 38
#define LW_HUD_Y 308
#define LW_HINT_Y 304
#define LW_BTN_X 131
#define LW_BTN_Y 147
#define LW_BTN_W 98
#define LW_BTN_H 66
#define LW_BTN_RADIUS 14
#define LW_LIMIT_W 280
#define LW_LIMIT_H 84
#define LW_LIMIT_MS 2000

/* Limit toast, 掷筊 format ("今日掷筊次数已用完"). */
#define LW_LIMIT_TEXT "今日转盘次数已用完"

typedef enum {
    LW_STAGE_START = 0,
    LW_STAGE_SPINNING,
    LW_STAGE_RESULT,
} lw_stage_t;

static lv_obj_t *s_page;
static lv_obj_t *s_back;
static lv_obj_t *s_title;
static lv_obj_t *s_wheel;
static lv_obj_t *s_wheel_box; /* round clip box around the disc */
static lv_obj_t *s_pointer;
static lv_obj_t *s_overlay; /* transparent full-page layer, START + RESULT */
static lv_obj_t *s_dim;     /* 50% black child of the overlay, RESULT only */
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_line1;
static lv_obj_t *s_btn_line2;
static lv_obj_t *s_hud;
static lv_obj_t *s_hint;
static lv_obj_t *s_limit_overlay; /* full-page dim + centred box, modal */
static lv_obj_t *s_limit_label;
static lv_timer_t *s_timer;
static uint32_t s_limit_since; /* lv_tick_get() when the toast showed, 0 = hidden */
static lw_stage_t s_stage = LW_STAGE_START;
static uint32_t s_angle10; /* current disc angle, 0..3599 */
static uint32_t s_spin_start10;
static uint32_t s_spin_total10;
static uint32_t s_spin_elapsed_ms;
static muse_luckywheel_result_t s_result = MUSE_LUCKYWHEEL_NONE;
static bool s_free;         /* the next spin is a free re-spin */
static bool s_pending_free; /* the in-flight request was free */
static bool s_seeded;
static bool s_built;

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void hide_limit(void)
{
    lv_obj_add_flag(s_limit_overlay, LV_OBJ_FLAG_HIDDEN);
    s_limit_since = 0;
}

static void show_limit(void)
{
    lv_label_set_text(s_limit_label, LW_LIMIT_TEXT);
    lv_obj_remove_flag(s_limit_overlay, LV_OBJ_FLAG_HIDDEN);
    s_limit_since = lv_tick_get();
}

static void on_limit_tap(lv_event_t *e)
{
    (void)e;
    /* Any tap on the prompt (box or dim) dismisses it; the tap never
     * reaches the page underneath, so it cannot start a spin. */
    hide_limit();
}

/* The prompt row (y=304) doubles as the note row: a refusal reason
 * replaces the "每次 -2 金币 · 今日已转 N/9 次" line until the next
 * visit, in DANGER. */
static void show_note(const char *text)
{
    lv_label_set_text(s_hint, text);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(MUSE_THEME_DANGER), 0);
    lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_hint(void)
{
    muse_pet_model_t snap;
    uint32_t remaining = MUSE_PET_LUCKYWHEEL_DAILY_MAX;
    if (muse_pet_snapshot(&snap)) {
        muse_pet_luckywheel_today(&snap, time(NULL), &remaining);
    }
    uint32_t used = MUSE_PET_LUCKYWHEEL_DAILY_MAX - remaining;
    char buf[48];
    snprintf(buf, sizeof(buf), "每次 -2 金币 · 今日已转 %u/9 次",
             (unsigned)used);
    lv_label_set_text(s_hint, buf);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

static void show_start(void)
{
    s_stage = LW_STAGE_START;
    lv_label_set_text(s_title, "幸运转盘");
    lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_btn_line1, "开始");
    lv_label_set_text(s_btn_line2, "");
    /* One line only on the START card, so centre it in the button. The
     * two-line layout (top +8 / bottom -8) belongs to the result cards,
     * where the fee line sits under the title (user report 2026-11-21:
     * "开始" sat high in the button). */
    lv_obj_align(s_btn_line1, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_btn_line2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
    refresh_hint();
    lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
}

static void show_result(void)
{
    s_stage = LW_STAGE_RESULT;
    lv_label_set_text(s_title, "游戏结束");
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Back to the two-line layout: the title line on top, the fee or
     * prize line under it. */
    lv_obj_align(s_btn_line1, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_remove_flag(s_btn_line2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
}

/* Cancel an in-flight spin (worker refusal): stop where the disc is,
 * back to the START card. A refused free spin is banked again, so the
 * "再来一次" the user won is not eaten by a storage error. */
static void cancel_spin(void)
{
    if (s_pending_free) {
        s_free = true;
        s_pending_free = false;
    }
    lv_image_set_rotation(s_wheel, (int16_t)s_angle10);
    show_start();
}

static void finish_spin(void)
{
    s_pending_free = false;
    uint32_t coins = muse_luckywheel_coins_for_result(s_result);
    if (s_result == MUSE_LUCKYWHEEL_AGAIN) {
        muse_audio_sfx(MUSE_SFX_DRAW);
        lv_label_set_text(s_hud, "再来一次，免费再转");
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD), 0);
        lv_label_set_text(s_btn_line1, "再来一次");
        lv_label_set_text(s_btn_line2, "免费");
        lv_obj_set_style_text_color(s_btn_line2,
                                    lv_color_hex(MUSE_THEME_GOLD), 0);
        /* Bank the free spin until the button is pressed. */
        s_free = true;
    } else if (coins > 0) {
        muse_audio_sfx(MUSE_SFX_WIN);
        char buf[32];
        snprintf(buf, sizeof(buf), "恭喜！+%u 金币", (unsigned)coins);
        lv_label_set_text(s_hud, buf);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD), 0);
        lv_label_set_text(s_btn_line1, "再来一次");
        lv_label_set_text(s_btn_line2, "-2 金币");
        lv_obj_set_style_text_color(s_btn_line2,
                                    lv_color_hex(MUSE_THEME_GOLD), 0);
    } else {
        muse_audio_sfx(MUSE_SFX_BAD);
        lv_label_set_text(s_hud, "谢谢参与");
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT), 0);
        lv_label_set_text(s_btn_line1, "再来一次");
        lv_label_set_text(s_btn_line2, "-2 金币");
        lv_obj_set_style_text_color(s_btn_line2,
                                    lv_color_hex(MUSE_THEME_GOLD), 0);
    }
    show_result();
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    if (s_limit_since != 0 &&
        lv_tick_get() - s_limit_since >= LW_LIMIT_MS) {
        hide_limit();
    }
    if (s_stage != LW_STAGE_SPINNING) {
        return;
    }
    s_spin_elapsed_ms += MUSE_LUCKYWHEEL_TICK_MS;
    if (s_spin_elapsed_ms >= MUSE_LUCKYWHEEL_SPIN_MS) {
        s_angle10 = (s_spin_start10 + s_spin_total10) % 3600u;
        lv_image_set_rotation(s_wheel, (int16_t)s_angle10);
        finish_spin();
        return;
    }
    uint32_t travelled =
        muse_luckywheel_angle_at(s_spin_elapsed_ms, s_spin_total10);
    lv_image_set_rotation(s_wheel,
                          (int16_t)((s_spin_start10 + travelled) % 3600u));
}

static void start_spin(void)
{
    if (s_stage == LW_STAGE_SPINNING) {
        return;
    }
    bool free = s_free;
    if (!free) {
        /* Snapshot gate: the worker is the authority, but the UI must
         * know before it starts the spin -- a spin the worker then
         * refuses would turn and cancel. Same order as 掷筊: limit
         * first, then coins. */
        muse_pet_model_t snap;
        if (muse_pet_snapshot(&snap)) {
            uint32_t remaining = 0;
            muse_pet_luckywheel_today(&snap, time(NULL), &remaining);
            if (remaining == 0) {
                show_limit();
                return;
            }
            if (snap.coins < MUSE_PET_LUCKYWHEEL_COST) {
                muse_audio_sfx(MUSE_SFX_BAD);
                show_note("金币不够（需 2 枚）");
                return;
            }
        }
    }
    if (!s_seeded) {
        srand((unsigned)lv_tick_get());
        s_seeded = true;
    }
    uint32_t segment = (uint32_t)rand() % MUSE_LUCKYWHEEL_SEGMENTS;
    s_result = muse_luckywheel_result_for_segment(segment);
    /* The request goes out before the animation starts (掷筊 rule):
     * the worker settles the economy, the spin only displays the
     * result that was drawn. A refused request cancels through the
     * feedback drain. */
    if (!muse_pet_request_luckywheel_spin((int32_t)s_result, free)) {
        show_note("互动未就绪");
        return;
    }
    s_free = false;
    s_pending_free = free;
    s_spin_start10 = s_angle10;
    s_spin_total10 = muse_luckywheel_total_angle(s_angle10, segment);
    s_spin_elapsed_ms = 0;
    s_stage = LW_STAGE_SPINNING;
    /* No overlay while spinning: the whole disc shows, the back arrow
     * hides (snake rule: the arrow is only on the overlay cards). */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
}

static void on_centre_button(lv_event_t *e)
{
    (void)e;
    start_spin();
}

void muse_luckywheel_ui_build(lv_obj_t *parent, lv_event_cb_t back_cb)
{
    if (s_built) {
        return;
    }
    s_built = true;
    s_page = parent;

    s_back = muse_back_button(s_page, back_cb);
    s_title = muse_page_title(s_page, "幸运转盘", LW_TITLE_Y);

    /* The disc: one RGB565 image, pivot at its centre, inside a round
     * clip box. The corners outside the disc are opaque black, and a
     * spinning 240 px square sweeps a 340 px circle -- it painted over
     * the purple title while turning (user report 2026-11-21). LVGL's
     * clip_corner only clips an object's CHILDREN, so the image is a
     * child of a circle-radius box: the sweep is cut at the rim and the
     * title stays clean. */
    s_wheel_box = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_wheel_box);
    lv_obj_set_size(s_wheel_box, MUSE_LUCKYWHEEL_ART_WHEEL_PX,
                    MUSE_LUCKYWHEEL_ART_WHEEL_PX);
    lv_obj_set_pos(s_wheel_box, LW_WHEEL_X, LW_WHEEL_Y);
    lv_obj_set_style_radius(s_wheel_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_wheel_box, true, 0);
    lv_obj_remove_flag(s_wheel_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_wheel_box, LV_OBJ_FLAG_CLICKABLE);

    s_wheel = lv_image_create(s_wheel_box);
    lv_image_set_src(s_wheel, &muse_luckywheel_art_wheel);
    lv_obj_set_size(s_wheel, MUSE_LUCKYWHEEL_ART_WHEEL_PX,
                    MUSE_LUCKYWHEEL_ART_WHEEL_PX);
    lv_obj_set_pos(s_wheel, 0, 0);
    lv_image_set_pivot(s_wheel, MUSE_LUCKYWHEEL_ART_WHEEL_PX / 2,
                       MUSE_LUCKYWHEEL_ART_WHEEL_PX / 2);
    lv_image_set_rotation(s_wheel, 0);
    lv_obj_remove_flag(s_wheel, LV_OBJ_FLAG_CLICKABLE);

    /* The pointer: a gold triangle that never moves. It is a later
     * sibling than the disc, so it draws on top of it. */
    s_pointer = lv_image_create(s_page);
    lv_image_set_src(s_pointer, &muse_luckywheel_art_pointer);
    lv_obj_set_size(s_pointer, MUSE_LUCKYWHEEL_ART_POINTER_W,
                    MUSE_LUCKYWHEEL_ART_POINTER_H);
    lv_obj_set_pos(s_pointer, LW_POINTER_X, LW_POINTER_Y);
    lv_obj_remove_flag(s_pointer, LV_OBJ_FLAG_CLICKABLE);

    /* Overlay layer (snake's overlay()): a transparent full-page
     * object holding the 50% dim child and the centre button. The
     * overlay itself is created after the disc and the pointer, so it
     * sits above them; the dim child is hidden on the START card. */
    s_overlay = lv_obj_create(s_page);
    lv_obj_set_size(s_overlay, 360, 360);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Transparent and NOT clickable: it is a drawing layer only. While
     * it was clickable this full-page box swallowed every tap, so "<"
     * (created before it) never fired -- the back arrow did nothing
     * (user report 2026-11-21). The centre button below is a child of
     * it and keeps its own clickable flag. */
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    s_dim = lv_obj_create(s_overlay);
    lv_obj_set_size(s_dim, 360, 360);
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_opa(s_dim, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_hex(MUSE_THEME_BG), 0);
    lv_obj_set_style_border_width(s_dim, 0, 0);
    lv_obj_set_style_radius(s_dim, 0, 0);
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);
    /* The dim is a look only: a clickable full-page child would swallow
     * the taps that belong to the button and to "<". */
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);

    /* Centre button, snake geometry: 98x66 at (131,147), CARD fill,
     * 2 px ACCENT border, radius 14, two lines of 16 px text. */
    s_btn = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_btn);
    lv_obj_set_size(s_btn, LW_BTN_W, LW_BTN_H);
    lv_obj_set_pos(s_btn, LW_BTN_X, LW_BTN_Y);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_btn, 2, 0);
    lv_obj_set_style_border_color(s_btn, lv_color_hex(MUSE_THEME_ACCENT), 0);
    lv_obj_set_style_radius(s_btn, LW_BTN_RADIUS, 0);
    lv_obj_add_event_cb(s_btn, on_centre_button, LV_EVENT_CLICKED, NULL);
    s_btn_line1 = mk_label(s_btn, muse_font_cjk(), MUSE_THEME_TEXT, "开始");
    lv_obj_align(s_btn_line1, LV_ALIGN_TOP_MID, 0, 8);
    s_btn_line2 = mk_label(s_btn, muse_font_cjk(), MUSE_THEME_GOLD, "");
    lv_obj_align(s_btn_line2, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* HUD (y=308, RESULT only) and the prompt row (y=304, START
     * only). Both are overlay children so they draw above the dim. */
    s_hud = mk_label(s_overlay, muse_font_cjk(), MUSE_THEME_GOLD, "");
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, LW_HUD_Y);
    lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
    s_hint = mk_label(s_overlay, muse_font_cjk(), MUSE_THEME_DIM, "");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, LW_HINT_Y);

    /* Limit toast (掷筊 prompt geometry): full-page dim + centred
     * 280x84 box. Created last so it sits on top of everything,
     * including "<" (a modal: while it is up, any tap dismisses it,
     * and it goes away on its own after ~2 s). Hidden until the day's
     * spins run out. */
    s_limit_overlay = lv_obj_create(s_page);
    lv_obj_set_size(s_limit_overlay, 360, 360);
    lv_obj_set_pos(s_limit_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_limit_overlay, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_limit_overlay, lv_color_hex(MUSE_THEME_BG), 0);
    lv_obj_set_style_border_width(s_limit_overlay, 0, 0);
    lv_obj_set_style_radius(s_limit_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_limit_overlay, 0, 0);
    lv_obj_remove_flag(s_limit_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_limit_overlay, on_limit_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_limit_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *box = lv_obj_create(s_limit_overlay);
    lv_obj_set_size(box, LW_LIMIT_W, LW_LIMIT_H);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(MUSE_THEME_ACCENT), 0);
    lv_obj_set_style_radius(box, LW_BTN_RADIUS, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    s_limit_label = mk_label(box, muse_font_cjk(), MUSE_THEME_TEXT,
                             LW_LIMIT_TEXT);
    lv_obj_center(s_limit_label);
    lv_obj_remove_flag(s_limit_label, LV_OBJ_FLAG_CLICKABLE);

    /* One timer for the page, paused until shown (掷筊 pattern). */
    s_timer = lv_timer_create(on_tick, MUSE_LUCKYWHEEL_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    show_start();
}

void muse_luckywheel_ui_show(void)
{
    if (!s_built) {
        return;
    }
    /* A visit always starts on the START card; the disc keeps its
     * landing angle. The free spin is banked in memory only -- leaving
     * the page with one banked loses it, same as leaving any game
     * mid-round. */
    s_pending_free = false;
    hide_limit();
    show_start();
    lv_timer_resume(s_timer);
}

void muse_luckywheel_ui_hide(void)
{
    if (!s_built) {
        return;
    }
    lv_timer_pause(s_timer);
    hide_limit();
    s_pending_free = false;
    show_start();
}

void muse_luckywheel_ui_fee_refused(void)
{
    if (!s_built) {
        return;
    }
    cancel_spin();
    muse_audio_sfx(MUSE_SFX_BAD);
    show_note("金币不够（需 2 枚）");
}

void muse_luckywheel_ui_limit_reached(void)
{
    if (!s_built) {
        return;
    }
    cancel_spin();
    show_limit();
}

void muse_luckywheel_ui_storage_error(void)
{
    if (!s_built) {
        return;
    }
    cancel_spin();
    show_note("互动数据损坏");
}

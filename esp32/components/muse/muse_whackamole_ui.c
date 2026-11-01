/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_whackamole_ui.c -- whack-a-mole subpage, see muse_whackamole_ui.h.
 *
 * Layout (mockup v9, 360x360 round screen, dark theme like the other
 * games): a 240x240 field at (60,60), six holes 3x2 on an 80px grid,
 * hole centres (100,180,260) x (140,220). Each hole is three stacked
 * images, drawn in this order: the hole image (72x40, soil rim outer
 * 36x20 + black pit inner 29x15), the mole image (56x47, top-left
 * cx-28,cy-31 so its bottom edge lands at cy+16), then the front lip
 * (72x40, the bottom-half rim band with the EXACT same radii as the
 * hole, drawn over the mole for the v9 3D look). The lip is never a
 * separate uniform-width arc: that was the v8 mockup flaw (1-2px
 * double edge where the arc did not match the rim's 7px x 5px band).
 *
 * The top crescent shows only the title "打地鼠" (y=38: the same slot and
 * ACCENT colour the snake page uses for its name); the "剩余 X秒 · 得分 Y"
 * line takes the slot right under the field (y=308, the slot the snake page
 * prints its score on), so it never rides on the bezel. While playing, the
 * top right crescent holds a small pause button (centre (300,26),
 * radius 17, two bars). There is no back arrow while playing; the back
 * arrow is the shared transparent one (56x48, TOP_MID -112,28,
 * montserrat LEFT symbol in ACCENT) the other mini games use, with no
 * filled background of its own.
 * A transparent full-page touch layer, visible only while playing,
 * maps presses to hole cells; outside the hole grid a press does
 * nothing. The START overlay is NOT dimmed (the field stays visible
 * behind it); the PAUSED and OVER overlays are 50% black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the mini
 * games): ACCENT title at y=100, a 110 px ACCENT-bordered centre circle,
 * and on the OVER card the price "- 5 金币" on its own line under the
 * replay label -- on_restart charges the fee the moment it is pressed, so
 * the price belongs on the button. The PAUSED card carries no restart
 * capsule: it charged 5 coins on a single tap in the narrowest part of the
 * bezel, and leaving is the back arrow's job (same decision as snake).
 */
#include "muse_whackamole_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_font.h"
#include "muse_pet.h"        /* snapshot + request: the entry fee */
#include "muse_pet_model.h"
#include "muse_theme.h"
#include "muse_whackamole.h"
#include "muse_whackamole_art.h"

#define MUSE_WHACKAMOLE_CELL 80
#define MUSE_WHACKAMOLE_FIELD_X 60
#define MUSE_WHACKAMOLE_FIELD_Y 60
#define MUSE_WHACKAMOLE_FIELD_SIZE 240
#define MUSE_WHACKAMOLE_HOLE_CX_0 100
#define MUSE_WHACKAMOLE_HOLE_CY_0 140
#define MUSE_WHACKAMOLE_HOLE_W 72
#define MUSE_WHACKAMOLE_HOLE_H 40
#define MUSE_WHACKAMOLE_MOLE_W 56
#define MUSE_WHACKAMOLE_MOLE_H 47

static muse_whackamole_t s_game;
static bool s_seeded = false;

static lv_obj_t *s_mole_img[MUSE_WHACKAMOLE_HOLES];
static lv_obj_t *s_lip_img[MUSE_WHACKAMOLE_HOLES];
static lv_obj_t *s_touch;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_paused_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_back;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_hud;
static lv_obj_t *s_note;
static lv_timer_t *s_timer;

/* --- small helpers, same shapes as the sudoku page ---------------- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *fill(lv_obj_t *parent, uint32_t bg)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void show_note(const char *text)
{
    lv_label_set_text(s_note, text);
    lv_obj_clear_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

static void hide_note(void)
{
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

/* --- display sync ------------------------------------------------- */

static void refresh_display(void)
{
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        muse_whackamole_hole_state_t st = s_game.holes[i].state;
        if (st == MUSE_WHACKAMOLE_EMPTY) {
            lv_obj_add_flag(s_mole_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lip_img[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_image_set_src(s_mole_img[i], st == MUSE_WHACKAMOLE_HIT
                                                  ? &muse_whackamole_mole_hit
                                                  : &muse_whackamole_mole);
            lv_obj_clear_flag(s_mole_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_lip_img[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char hud[48];
    snprintf(hud, sizeof(hud), "剩余 %lus · 得分 %lu",
             (unsigned long)muse_whackamole_remaining_s(&s_game),
             (unsigned long)s_game.score);
    lv_label_set_text(s_hud, hud);
    /* No per-game breakdown on the OVER card any more (2026-11-01, user
     * request): the result line under the field already carries the
     * score, and the hits/missed split only crowded the overlay. */
    muse_whackamole_phase_t ph = s_game.phase;
    if (ph == MUSE_WHACKAMOLE_START) {
        lv_obj_clear_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_WHACKAMOLE_PAUSED) {
        lv_obj_clear_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_WHACKAMOLE_OVER) {
        lv_obj_clear_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Touch only while playing; pause button only while playing;
     * back arrow on every overlay (never while playing). */
    if (ph == MUSE_WHACKAMOLE_PLAYING) {
        lv_obj_clear_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- entry fee (same snapshot gate + PET_FEE pattern as snake) ---- */

static bool charge_for_game(void)
{
    muse_pet_model_t snap;
    if (!muse_pet_snapshot(&snap)) {
        show_note("互动未就绪");
        return false;
    }
    if (snap.coins < MUSE_PET_GAME_STAKE) {
        show_note("金币不够，先摸摸 / 喂饭 / 抽运势");
        return false;
    }
    if (!muse_pet_request(PET_FEE)) {
        show_note("互动未就绪");
        return false;
    }
    hide_note();
    return true;
}

static void seed_once(void)
{
    if (!s_seeded) {
        srand(lv_tick_get());
        s_seeded = true;
    }
}

/* --- game actions ------------------------------------------------- */

static void begin_game(bool restart)
{
    if (!charge_for_game()) {
        return;
    }
    seed_once();
    if (restart) {
        muse_whackamole_restart(&s_game);
    } else {
        muse_whackamole_start(&s_game);
    }
    lv_timer_resume(s_timer);
    refresh_display();
}

static void on_start(lv_event_t *e)
{
    (void)e;
    begin_game(false);
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    begin_game(true);
}

static void on_resume(lv_event_t *e)
{
    (void)e;
    muse_whackamole_resume(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void on_pause(lv_event_t *e)
{
    (void)e;
    muse_whackamole_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_WHACKAMOLE_PLAYING) {
        return;
    }
    muse_whackamole_tick(&s_game, MUSE_WHACKAMOLE_TICK_MS);
    if (s_game.phase == MUSE_WHACKAMOLE_OVER) {
        lv_timer_pause(s_timer);
    }
    refresh_display();
}

static void on_touch(lv_event_t *e)
{
    (void)e;
    if (s_game.phase != MUSE_WHACKAMOLE_PLAYING) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    /* The page group sits at the screen origin while the tile is
     * active, so the indev point is already page coordinates. */
    if (point.x < MUSE_WHACKAMOLE_FIELD_X ||
        point.x >= MUSE_WHACKAMOLE_FIELD_X + MUSE_WHACKAMOLE_FIELD_SIZE ||
        point.y < MUSE_WHACKAMOLE_HOLE_CY_0 - MUSE_WHACKAMOLE_CELL / 2 ||
        point.y >= MUSE_WHACKAMOLE_HOLE_CY_0 - MUSE_WHACKAMOLE_CELL / 2 +
                       MUSE_WHACKAMOLE_ROWS * MUSE_WHACKAMOLE_CELL) {
        return; /* top/bottom crescents and side margins: no hole */
    }
    int col = (point.x - MUSE_WHACKAMOLE_FIELD_X) / MUSE_WHACKAMOLE_CELL;
    int row = (point.y - (MUSE_WHACKAMOLE_HOLE_CY_0 - MUSE_WHACKAMOLE_CELL / 2)) /
              MUSE_WHACKAMOLE_CELL;
    if (col < 0 || col >= MUSE_WHACKAMOLE_COLS || row < 0 ||
        row >= MUSE_WHACKAMOLE_ROWS) {
        return;
    }
    if (muse_whackamole_tap(&s_game, row * MUSE_WHACKAMOLE_COLS + col)) {
        refresh_display();
    }
}

/* --- build -------------------------------------------------------- */

static lv_obj_t *overlay(lv_obj_t *page, bool dim)
{
    lv_obj_t *ov = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(ov, 360, 360);
    lv_obj_set_pos(ov, 0, 0);
    if (dim) {
        lv_obj_set_style_bg_color(ov, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ov, LV_OPA_50, LV_PART_MAIN);
    } else {
        /* Undimmed START overlay: transparent, so the field stays
         * visible behind the start button. */
        lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    return ov;
}

static lv_obj_t *round_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 110, 110);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(MUSE_THEME_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(MUSE_THEME_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    return btn;
}

void muse_whackamole_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_whackamole_init(&s_game);

    /* Field, then per-hole stacks: hole -> mole -> lip. */
    lv_obj_t *field = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(field, MUSE_WHACKAMOLE_FIELD_SIZE,
                    MUSE_WHACKAMOLE_FIELD_SIZE);
    lv_obj_set_pos(field, MUSE_WHACKAMOLE_FIELD_X, MUSE_WHACKAMOLE_FIELD_Y);
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        int col = i % MUSE_WHACKAMOLE_COLS;
        int row = i / MUSE_WHACKAMOLE_COLS;
        int cx = MUSE_WHACKAMOLE_HOLE_CX_0 + col * MUSE_WHACKAMOLE_CELL;
        int cy = MUSE_WHACKAMOLE_HOLE_CY_0 + row * MUSE_WHACKAMOLE_CELL;
        lv_obj_t *hole = lv_image_create(page);
        lv_image_set_src(hole, &muse_whackamole_hole);
        lv_obj_set_pos(hole, cx - MUSE_WHACKAMOLE_HOLE_W / 2,
                       cy - MUSE_WHACKAMOLE_HOLE_H / 2);
        lv_obj_remove_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(hole, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *mole = lv_image_create(page);
        lv_image_set_src(mole, &muse_whackamole_mole);
        lv_obj_set_pos(mole, cx - MUSE_WHACKAMOLE_MOLE_W / 2,
                       cy - MUSE_WHACKAMOLE_MOLE_H + 16);
        lv_obj_remove_flag(mole, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(mole, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(mole, LV_OBJ_FLAG_HIDDEN);
        s_mole_img[i] = mole;
        lv_obj_t *lip = lv_image_create(page);
        lv_image_set_src(lip, &muse_whackamole_lip);
        lv_obj_set_pos(lip, cx - MUSE_WHACKAMOLE_HOLE_W / 2,
                       cy - MUSE_WHACKAMOLE_HOLE_H / 2);
        lv_obj_remove_flag(lip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(lip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(lip, LV_OBJ_FLAG_HIDDEN);
        s_lip_img[i] = lip;
    }

    /* Touch layer: full page, only while playing, above the field.
     * fill() strips CLICKABLE, so it is added back: without it the
     * layer would never receive PRESSED events. */
    s_touch = fill(page, 0x000000);
    lv_obj_set_style_bg_opa(s_touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(s_touch, 360, 360);
    lv_obj_set_pos(s_touch, 0, 0);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_touch, on_touch, LV_EVENT_PRESSED, NULL);

    /* START overlay: not dimmed, the field stays visible behind it. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = round_btn(s_start_ov, 125, 125);
    lv_obj_t *start_lb = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(start_lb, "开始");
    lv_obj_center(start_lb);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);

    /* PAUSED overlay: dim, ACCENT title, 继续 circle with the PLAY symbol
     * above the label (snake's card). No restart capsule: it charged 5
     * coins on a single tap in the narrowest part of the bezel; the back
     * arrow leaves, and 开始 charges again on the next visit. */
    s_paused_ov = overlay(page, true);
    lv_obj_t *paused_lb = label(s_paused_ov, muse_font_cjk(), MUSE_THEME_ACCENT);
    lv_label_set_text(paused_lb, "已暂停");
    lv_obj_align(paused_lb, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_t *resume_btn = round_btn(s_paused_ov, 125, 125);
    lv_obj_t *resume_sym = label(resume_btn, &lv_font_montserrat_20, MUSE_THEME_TEXT);
    lv_label_set_text(resume_sym, LV_SYMBOL_PLAY);
    lv_obj_align(resume_sym, LV_ALIGN_TOP_MID, 0, 31);
    lv_obj_t *resume_lb = label(resume_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(resume_lb, "继续");
    lv_obj_align(resume_lb, LV_ALIGN_TOP_MID, 0, 59);
    lv_obj_add_event_cb(resume_btn, on_resume, LV_EVENT_CLICKED, NULL);

    /* OVER overlay: dim, ACCENT title, 再玩一次 circle with the price on
     * its own line under the label (snake's card). No hits/missed split
     * here any more: the result line under the field carries the score. */
    s_over_ov = overlay(page, true);
    lv_obj_t *over_lb = label(s_over_ov, muse_font_cjk(), MUSE_THEME_ACCENT);
    lv_label_set_text(over_lb, "游戏结束");
    lv_obj_align(over_lb, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_t *again_btn = round_btn(s_over_ov, 125, 125);
    lv_obj_t *again_lb = label(again_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(again_lb, "再玩一次");
    lv_obj_align(again_lb, LV_ALIGN_TOP_MID, 0, 32);
    /* on_restart charges through charge_for_game() the moment it is
     * pressed, so the price belongs on the button (same as snake). */
    lv_obj_t *again_price = label(again_btn, muse_font_cjk(), MUSE_THEME_GOLD);
    lv_label_set_text(again_price, "- 5 金币");
    lv_obj_align(again_price, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_event_cb(again_btn, on_restart, LV_EVENT_CLICKED, NULL);

    /* Back arrow: the shared transparent one (no filled background, user
     * request 2026-11-01), same geometry and symbol as snake / sudoku.
     * Hidden while playing. Created after the overlays so it stays
     * clickable on them. */
    s_back = lv_obj_create(page);
    lv_obj_remove_style_all(s_back);
    lv_obj_set_size(s_back, 56, 48);
    lv_obj_align(s_back, LV_ALIGN_TOP_MID, -112, 28);
    lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lb = label(s_back, &lv_font_montserrat_20, MUSE_THEME_ACCENT);
    lv_label_set_text(back_lb, LV_SYMBOL_LEFT);
    lv_obj_center(back_lb);

    /* Pause button, top-right crescent, only while playing. */
    s_pause_btn = lv_btn_create(page);
    lv_obj_set_size(s_pause_btn, 34, 34);
    lv_obj_set_pos(s_pause_btn, 283, 9);
    lv_obj_set_style_bg_color(s_pause_btn, lv_color_hex(MUSE_THEME_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pause_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pause_btn, lv_color_hex(MUSE_THEME_RULE), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pause_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pause_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_pause_btn, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_pause_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *bar1 = fill(s_pause_btn, MUSE_THEME_TEXT);
    lv_obj_set_size(bar1, 4, 16);
    lv_obj_set_pos(bar1, 11, 9);
    lv_obj_t *bar2 = fill(s_pause_btn, MUSE_THEME_TEXT);
    lv_obj_set_size(bar2, 4, 16);
    lv_obj_set_pos(bar2, 19, 9);
    lv_obj_add_event_cb(s_pause_btn, on_pause, LV_EVENT_CLICKED, NULL);

    /* Title (top crescent, y=38: snake's name slot) and the result line
     * right under the field (y=308, snake's score slot): page-level, above
     * the dim overlays, so they stay readable in every state. The line used
     * to sit at y=332 and rode on the bezel (user report 2026-11-01). */
    lv_obj_t *title = label(page, muse_font_cjk(), MUSE_THEME_ACCENT);
    lv_label_set_text(title, "打地鼠");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, 308);

    /* Refusal note: same slot as snake's (y=280, in the empty band under
     * the second hole row). */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, 280);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_WHACKAMOLE_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_whackamole_ui_show(void)
{
    refresh_display();
}

void muse_whackamole_ui_hide(void)
{
    if (s_game.phase == MUSE_WHACKAMOLE_PLAYING) {
        muse_whackamole_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    refresh_display();
}

void muse_whackamole_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_WHACKAMOLE_PLAYING ||
        s_game.phase == MUSE_WHACKAMOLE_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_whackamole_init(&s_game);
        lv_timer_pause(s_timer);
    }
    show_note("金币不够，先摸摸 / 喂饭 / 抽运势");
    refresh_display();
}

/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_mathpractice_ui.c -- 加减练习 subpage, see muse_mathpractice_ui.h.
 *
 * Layout (mockup v3, 360x360 round screen, dark theme like the other
 * games): the title "加减练习" sits in the top crescent (y=38, the
 * same slot and ACCENT colour the snake page uses for its name); the
 * equation ("7 + 8 = 15") is one large line at y=96, in Montserrat 28
 * (the digits and + - = are ASCII, so they do not need the 16 px Noto
 * body font -- user 2026-11-08: "数字再大一些，16px 放到 20px",
 * taken to 28 px, the largest Montserrat in the build); the two
 * answer buttons sit below it at y=200, 100x80 each -- the green
 * check on the left (x=60) and the red cross on the right (x=200) --
 * solid fills with white LVGL symbols. The line under the buttons
 * (y=308, the slot the snake page prints its score on) never rides
 * on the bezel: while PLAYING or PAUSED it is white and reads "本局
 * n 分 · 剩余 s 秒", and at OVER it turns GOLD and reads "本局 n 分，
 * +m 金币" with m the gross payout. There is no pause button any
 * more (2026-11-01, user request "参考贪吃蛇"): pausing is a LONG
 * HOLD on the equation area (finger within ~12 px for 500 ms, the
 * doodle page's idiom), and the back arrow (which is hidden while
 * playing, as on snake) becomes reachable through it. The START
 * overlay is NOT dimmed (the equation stays visible behind it); the
 * PAUSED and OVER overlays are 50% black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the
 * mini games): the PAUSED card has an ACCENT title at y=100 and a
 * 98x66 ACCENT-bordered centre button, and the OVER card has the
 * price "- 5 金币" on its own line under the replay label -- on_restart
 * charges the fee the moment it is pressed, so the price belongs on
 * the button. The OVER card itself carries no title and no payout line
 * any more (UI polish 2026-11-01): both moved to the page-level title
 * and HUD slots. The PAUSED card carries no restart capsule: it
 * charged 5 coins on a single tap in the narrowest part of the bezel,
 * and leaving is the back arrow's job (same decision as snake). The
 * START overlay carries the hint line at y=304 (DIM, same slot as the
 * snake page's hint): "绿勾是对，红叉是错".
 *
 * Payout (same as snake / whack-a-mole / goldrain / doodle): at OVER
 * the HUD turns gold and reads "本局 n 分，+m 金币" with m the GROSS
 * payout the wallet actually accepted (s_round_coins, from
 * muse_pet_round_coins() -- 1 point per coin, 10 coins per round
 * ceiling; the score is settled once, at OVER). The net is
 * deliberately NOT printed: the user said kids cannot tell net apart,
 * so the result shows what was added, not what was gained after the
 * 5-coin entry fee. The score only ever goes up (a wrong judgement
 * is +0, never -1), so the HUD never shows a negative.
 *
 * Message slot: the band under the buttons (y=308) hosts exactly one
 * line -- the refusal note wins it, otherwise the score line does.
 * The note used to sit at y=280, inside the field; it now shares the
 * score line's slot, which is why it carries the short copy (the chord
 * under the field is only ~213 px at y=325).
 *
 * Controls (user decision 2026-11-08): a tap on the green check
 * judges the equation right, a tap on the red cross judges it wrong;
 * either tap deals the next equation at once. A press that moves more
 * than ~12 px is a drag and can never pause, so a lingering drag does
 * not stop the game by accident; a press held still for 500 ms on the
 * equation area pauses. The hold is tracked by hand, not LVGL's
 * LONG_PRESSED, and the hold layer sits UNDER the answer buttons, so
 * a tap on a button never also pauses. The back arrow stays hidden
 * while playing -- hold first, then leave from the pause card.
 *
 * Power: no sensor is used (unlike 金币雨 / 毛毛跳跃, there is no
 * tilt here), so there is nothing to wake or sleep. A running round
 * pokes the idle timer (muse_state_poke) because a fast player may
 * answer for seconds without the screen sleeping mid-round (same
 * idiom as 金币雨 and video playback).
 */
#include "muse_mathpractice_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_font.h"
#include "muse_widgets.h"
#include "muse_pet.h"        /* snapshot + request: the entry fee */
#include "muse_pet_model.h"
#include "muse_state.h"
#include "muse_theme.h"
#include "muse_mathpractice.h"

/* Answer button geometry (the mockup v3): 100x80, green check left,
 * red cross right, both at y=200. */
#define MUSE_MATHPRACTICE_BTN_W 100
#define MUSE_MATHPRACTICE_BTN_H 80
#define MUSE_MATHPRACTICE_BTN_Y 200
#define MUSE_MATHPRACTICE_BTN_CHECK_X 60
#define MUSE_MATHPRACTICE_BTN_CROSS_X 200
/* Solid fills (the mockup v3): a saturated green and red so the white
 * symbols read at a glance. The theme OK / DANGER are paler tints
 * meant for text, not for a filled button. */
#define MUSE_MATHPRACTICE_GREEN_HEX 0x2ec27a
#define MUSE_MATHPRACTICE_RED_HEX 0xe5484d
/* Equation line (the mockup v3): Montserrat 28 at y=96. The digits
 * and + - = are ASCII, so they do not need the 16 px Noto body font. */
#define MUSE_MATHPRACTICE_EQ_Y 96
/* Hold to pause: the finger stays within this radius for this long,
 * on the equation area only (the doodle page's numbers). */
#define MUSE_MATHPRACTICE_HOLD_MS 500u
#define MUSE_MATHPRACTICE_HOLD_MOVE_PX 12
/* The hold layer covers the equation zone, y=60..190: above the
 * answer buttons, so a tap on a button never starts a hold. */
#define MUSE_MATHPRACTICE_HOLD_Y 60
#define MUSE_MATHPRACTICE_HOLD_H 130

static muse_mathpractice_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the snake page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* Press tracking for the hold-to-pause (equation area only). */
static bool s_press_active = false;
static bool s_press_moved = false;
static uint32_t s_press_start_ms = 0;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;

static lv_obj_t *s_eq;
static lv_obj_t *s_check_btn;
static lv_obj_t *s_cross_btn;
static lv_obj_t *s_hold;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_paused_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_back;
static lv_obj_t *s_name;
static lv_obj_t *s_hud;
static lv_obj_t *s_note;
static lv_timer_t *s_timer;

/* Status / message slot under the buttons: the HUD and the refusal
 * note share it, one at a time, so both are below the play area (user
 * request 2026-11-01). ~213 px of chord at the line's bottom (325)
 * and the longest string here ("本局 n 分，+m 金币", ~180 px) still
 * clears the bezel. */
#define MUSE_MATHPRACTICE_HUD_Y 308
#define MUSE_MATHPRACTICE_NOTE_Y 308

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
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 360, 360);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void sync_message_band(void)
{
    bool note_shown = !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    if (note_shown) {
        lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_label(lv_obj_t *l, const char *text)
{
    lv_label_set_text(l, text);
}

static void show_note(const char *text)
{
    set_label(s_note, text);
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

static void hide_note(void)
{
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

static void refresh_display(void)
{
    /* Equation line: "a + b = shown" or "a - b = shown", large. */
    char eq_buf[32];
    snprintf(eq_buf, sizeof(eq_buf), "%d %c %d = %d", s_game.a,
             s_game.is_add ? '+' : '-', s_game.b, s_game.shown);
    set_label(s_eq, eq_buf);
    /* HUD: score + time while playing/paused, gross payout at OVER. */
    char hud_buf[48];
    if (s_game.phase == MUSE_MATHPRACTICE_OVER) {
        snprintf(hud_buf, sizeof(hud_buf), "本局 %u 分，+%u 金币",
                 (unsigned)s_game.score, (unsigned)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud_buf, sizeof(hud_buf), "本局 %u 分 · 剩余 %u 秒",
                 (unsigned)s_game.score,
                 (unsigned)muse_mathpractice_remaining_s(&s_game));
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud_buf);
    /* Title: the game name, "已暂停" while paused, or "游戏结束"
     * at OVER (user 2026-11-08: the pause title moves to the title
     * slot on every game, so the pause card is just the 继续 button). */
    set_label(s_name, s_game.phase == MUSE_MATHPRACTICE_OVER ? "游戏结束"
                     : s_game.phase == MUSE_MATHPRACTICE_PAUSED ? "已暂停"
                                                             : "加减练习");
    /* Overlays: exactly one visible at a time. */
    if (s_game.phase == MUSE_MATHPRACTICE_START) {
        lv_obj_remove_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else if (s_game.phase == MUSE_MATHPRACTICE_PAUSED) {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else if (s_game.phase == MUSE_MATHPRACTICE_OVER) {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Answer buttons and the equation only take input while playing;
     * the hold layer only while playing too. The back arrow is hidden
     * while playing, as on snake. */
    bool playing = (s_game.phase == MUSE_MATHPRACTICE_PLAYING);
    if (playing) {
        lv_obj_remove_flag(s_hold, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_hold, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_opa(s_check_btn, playing ? LV_OPA_COVER : LV_OPA_50,
                         LV_PART_MAIN);
    lv_obj_set_style_opa(s_cross_btn, playing ? LV_OPA_COVER : LV_OPA_50,
                         LV_PART_MAIN);
}

/* --- economy, same shape as the doodle page ---------------------- */

static bool charge_for_game(void)
{
    muse_pet_model_t snap;
    if (!muse_pet_snapshot(&snap)) {
        show_note("互动未就绪");
        return false;
    }
    if (snap.coins < MUSE_PET_GAME_STAKE) {
        /* Short copy on purpose (see MUSE_MATHPRACTICE_NOTE_Y): the slot
         * under the buttons leaves ~213 px of chord and the long sentence
         * is ~272 px wide. Same wording as the sudoku page's note. */
        char note[32];
        snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
                 (unsigned)MUSE_PET_GAME_STAKE);
        show_note(note);
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
    /* A new round: its payout has not been settled yet. */
    s_settled = false;
    s_round_coins = 0;
    if (restart) {
        muse_mathpractice_restart(&s_game);
    } else {
        muse_mathpractice_start(&s_game);
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
    muse_mathpractice_resume(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void pause_game(void)
{
    muse_mathpractice_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 10 the same day). muse_pet_round_coins() is the
 * only conversion curve -- 1 point per correct judgement, 10 coins
 * per round ceiling -- and the 5-coin entry fee was already charged
 * at start through PET_FEE, so the round's net is payout minus 5,
 * shown on the OVER card. The score only ever goes up (a wrong
 * judgement is +0), so no floor helper is needed -- round_coins()
 * takes an unsigned score. A refused request (worker not ready /
 * queue full) leaves s_round_coins at 0 so the card never claims
 * coins the wallet did not get. */
static void settle_round(void)
{
    if (s_settled) {
        return;
    }
    s_settled = true;
    uint32_t coins = muse_pet_round_coins(s_game.score);
    s_round_coins = 0;
    if (coins != 0 && muse_pet_request_play_reward(coins)) {
        s_round_coins = coins;
    }
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_MATHPRACTICE_PLAYING) {
        return;
    }
    /* A running round is activity: a fast player may answer for
     * seconds without the screen sleeping mid-round (same idiom as
     * 金币雨 and video playback). */
    muse_state_poke();
    /* Hold to pause: the finger stayed within ~12 px for 500 ms on
     * the equation area. A drag that lingers never reaches this branch
     * (s_press_moved). */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms >= MUSE_MATHPRACTICE_HOLD_MS) {
        s_press_active = false;
        pause_game();
        return;
    }
    muse_mathpractice_tick(&s_game, MUSE_MATHPRACTICE_TICK_MS);
    if (s_game.phase == MUSE_MATHPRACTICE_OVER) {
        settle_round();
        lv_timer_pause(s_timer);
    }
    refresh_display();
}

/* --- answer buttons ------------------------------------------------ */

static void on_check(lv_event_t *e)
{
    (void)e;
    if (s_game.phase != MUSE_MATHPRACTICE_PLAYING) {
        return;
    }
    muse_mathpractice_answer(&s_game, true);
    refresh_display();
}

static void on_cross(lv_event_t *e)
{
    (void)e;
    if (s_game.phase != MUSE_MATHPRACTICE_PLAYING) {
        return;
    }
    muse_mathpractice_answer(&s_game, false);
    refresh_display();
}

/* --- hold to pause (equation area only) --------------------------- */

static void on_hold_pressed(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    s_press_active = true;
    s_press_moved = false;
    s_press_start_ms = lv_tick_get();
    s_press_x = p.x;
    s_press_y = p.y;
}

static void on_hold_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_press_active) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (abs(p.x - s_press_x) > MUSE_MATHPRACTICE_HOLD_MOVE_PX ||
        abs(p.y - s_press_y) > MUSE_MATHPRACTICE_HOLD_MOVE_PX) {
        s_press_moved = true;
    }
}

static void on_hold_released(lv_event_t *e)
{
    (void)e;
    s_press_active = false;
    s_press_moved = false;
}

/* --- page construction -------------------------------------------- */

static lv_obj_t *overlay(lv_obj_t *page, bool dim)
{
    lv_obj_t *ov = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(ov, 360, 360);
    lv_obj_set_pos(ov, 0, 0);
    if (dim) {
        lv_obj_set_style_bg_color(ov, lv_color_hex(MUSE_THEME_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ov, LV_OPA_50, LV_PART_MAIN);
    } else {
        /* Undimmed START overlay: transparent, so the equation stays
         * visible behind the start button. */
        lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    return ov;
}

static lv_obj_t *centre_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    /* The shared card, a 98x66 rounded rect (user decision 2026-11-01,
     * was a 110 px circle): the ACCENT ring is this page's own accent on
     * top of the shared CARD / CARD_PRESSED base. */
    lv_obj_t *btn = muse_card(parent, 98, 66, 14);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_border_color(btn, lv_color_hex(MUSE_THEME_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    return btn;
}

/* One answer button: a 100x80 solid fill with a white symbol. The
 * symbol is an LVGL FontAwesome glyph (LV_SYMBOL_OK / LV_SYMBOL_CLOSE)
 * in Montserrat 28, so it scales with the button. */
static lv_obj_t *answer_btn(lv_obj_t *page, lv_coord_t x, uint32_t fill_hex,
                            const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(page);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, MUSE_MATHPRACTICE_BTN_W, MUSE_MATHPRACTICE_BTN_H);
    lv_obj_set_pos(btn, x, MUSE_MATHPRACTICE_BTN_Y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(fill_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sym = label(btn, &lv_font_montserrat_28, 0xffffff);
    lv_label_set_text(sym, symbol);
    lv_obj_center(sym);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void muse_mathpractice_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_mathpractice_init(&s_game);

    /* Equation line: Montserrat 28, centred at y=96 (the mockup v3).
     * Created before the buttons so the buttons sit on top of it. */
    s_eq = label(page, &lv_font_montserrat_28, MUSE_THEME_TEXT);
    lv_obj_align(s_eq, LV_ALIGN_TOP_MID, 0, MUSE_MATHPRACTICE_EQ_Y);

    /* Answer buttons: green check left, red cross right. */
    s_check_btn = answer_btn(page, MUSE_MATHPRACTICE_BTN_CHECK_X,
                             MUSE_MATHPRACTICE_GREEN_HEX, LV_SYMBOL_OK,
                             on_check);
    s_cross_btn = answer_btn(page, MUSE_MATHPRACTICE_BTN_CROSS_X,
                             MUSE_MATHPRACTICE_RED_HEX, LV_SYMBOL_CLOSE,
                             on_cross);

    /* Hold-to-pause layer: transparent, over the equation zone only
     * (y=60..190), UNDER the answer buttons -- it is created after the
     * buttons would put it on top of them, so it goes here and the
     * buttons are re-raised above it. A tap on a button never starts
     * a hold. */
    s_hold = fill(page, MUSE_THEME_BG);
    lv_obj_set_style_bg_opa(s_hold, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(s_hold, 360, MUSE_MATHPRACTICE_HOLD_H);
    lv_obj_set_pos(s_hold, 0, MUSE_MATHPRACTICE_HOLD_Y);
    lv_obj_add_flag(s_hold, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_hold, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_hold, on_hold_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_hold, on_hold_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_hold, on_hold_released, LV_EVENT_RELEASED, NULL);
    lv_obj_move_foreground(s_check_btn);
    lv_obj_move_foreground(s_cross_btn);

    /* START overlay: not dimmed, the equation stays visible behind it. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = centre_btn(s_start_ov, 131, 147);
    lv_obj_t *start_lb = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(start_lb, "开始");
    lv_obj_center(start_lb);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    /* Hint line (mockup v3): same slot as the snake page's hint. */
    lv_obj_t *hint_lb = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint_lb, "绿勾是对，红叉是错");
    lv_obj_align(hint_lb, LV_ALIGN_TOP_MID, 0, 304);

    /* PAUSED overlay: dim, 继续 button with the PLAY symbol above
     * the label (snake's card). No "已暂停" label on the card any
     * more: the page title itself becomes "已暂停" while paused
     * (user 2026-11-08). No restart capsule: it charged 5 coins on
     * a single tap in the narrowest part of the bezel; the back
     * arrow leaves, and 开始 charges again on the next visit. */
    s_paused_ov = overlay(page, true);
    lv_obj_t *resume_btn = centre_btn(s_paused_ov, 131, 147);
    lv_obj_t *resume_sym = label(resume_btn, &lv_font_montserrat_20, MUSE_THEME_TEXT);
    lv_label_set_text(resume_sym, LV_SYMBOL_PLAY);
    lv_obj_align(resume_sym, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *resume_lb = label(resume_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(resume_lb, "继续");
    lv_obj_align(resume_lb, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_event_cb(resume_btn, on_resume, LV_EVENT_CLICKED, NULL);

    /* OVER overlay: dim + 再来一局 button only (UI polish 2026-11-01).
     * The result title lives in the page-title slot and the result line
     * under the buttons -- both page-level, above the dim -- so the overlay
     * itself carries no title and no payout line any more. */
    s_over_ov = overlay(page, true);
    lv_obj_t *again_btn = centre_btn(s_over_ov, 131, 147);
    lv_obj_t *again_lb = label(again_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(again_lb, "再来一局");
    lv_obj_align(again_lb, LV_ALIGN_TOP_MID, 0, 12);
    /* on_restart charges through charge_for_game() the moment it is
     * pressed, so the price belongs on the button (same as snake; the
     * amount comes from the macro, not a literal). */
    char price_buf[16];
    snprintf(price_buf, sizeof(price_buf), "- %u 金币",
             (unsigned)MUSE_PET_GAME_STAKE);
    lv_obj_t *again_price = label(again_btn, muse_font_cjk(), MUSE_THEME_GOLD);
    lv_label_set_text(again_price, price_buf);
    lv_obj_align(again_price, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_event_cb(again_btn, on_restart, LV_EVENT_CLICKED, NULL);

    /* Back arrow: the shared transparent one (no filled background, user
     * request 2026-11-01), same geometry and symbol as snake / sudoku.
     * Hidden while playing. Created after the overlays so it stays
     * clickable on them. */
    s_back = muse_back_button(page, back_cb);

    /* Title (top crescent, y=38: snake's name slot, shared subpage title
     * font/colour) and the result line right under the buttons
     * (MUSE_MATHPRACTICE_HUD_Y = 308, snake's score slot): page-level,
     * above the dim overlays, so they stay readable in every state.
     * refresh_display() swaps the title to "游戏结束" at OVER. */
    s_name = muse_page_title(page, "加减练习", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_MATHPRACTICE_HUD_Y);

    /* Refusal note: below the buttons, sharing the score line's slot
     * (user request 2026-11-01). */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_MATHPRACTICE_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_MATHPRACTICE_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_mathpractice_ui_show(void)
{
    refresh_display();
}

void muse_mathpractice_ui_hide(void)
{
    if (s_game.phase == MUSE_MATHPRACTICE_PLAYING) {
        muse_mathpractice_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    s_press_active = false;
    refresh_display();
}

void muse_mathpractice_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_MATHPRACTICE_PLAYING ||
        s_game.phase == MUSE_MATHPRACTICE_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_mathpractice_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}

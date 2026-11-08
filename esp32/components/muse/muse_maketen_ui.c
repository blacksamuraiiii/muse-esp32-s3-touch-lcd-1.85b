/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_maketen_ui.c -- 凑十法练习 subpage, see muse_maketen_ui.h.
 *
 * Layout (mockup v4, 360x360 round screen, dark theme like the other
 * games): the title "凑十法练习" sits in the top crescent (y=38, the
 * same slot and ACCENT colour the snake page uses for its name); the
 * question ("5 + 8 ? 13") is one large line at y=96, in Montserrat 28
 * (the digits and + = ? are ASCII, so they do not need the 16 px Noto
 * body font -- same choice as 加减练习, user 2026-11-08: "数字再大
 * 一些，16px 放到 20px", taken to 28 px); the make-ten hint
 * ("提示：2+3=5") sits under it at y=150, in the 16 px Noto body
 * font and DIM -- the mockup draws it larger (26 px), but the build
 * has no larger CJK font, so the hint renders at body size. The
 * three comparison buttons sit below it at y=210, 90x64 each -- >,
 * < and = in that order -- CARD fill with an ACCENT border and a
 * TEXT symbol. The line
 * under the buttons (y=308, the slot the snake page prints its score
 * on) never rides on the bezel: while PLAYING or PAUSED it is white
 * and reads "本局 n 分 · 剩余 s 秒", and at OVER it turns GOLD and
 * reads "本局 n 分，+m 金币" with m the gross payout. There is no
 * pause button any more (2026-11-01, user request "参考贪吃蛇"):
 * pausing is a LONG HOLD on the question area (finger within ~12 px
 * for 500 ms, the doodle page's idiom), and the back arrow (which is
 * hidden while playing, as on snake) becomes reachable through it.
 * The START overlay is NOT dimmed (the question stays visible behind
 * it); the PAUSED and OVER overlays are 50% black dim.
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
 * snake page's hint): "算出左边，再比大小".
 *
 * Payout (same as snake / whack-a-mole / goldrain / doodle): at OVER
 * the HUD turns gold and reads "本局 n 分，+m 金币" with m the GROSS
 * payout the wallet actually accepted (s_round_coins, from
 * muse_pet_round_coins() -- 1 point per coin, 10 coins per round
 * ceiling; the score is settled once, at OVER). The net is
 * deliberately NOT printed: the user said kids cannot tell net apart,
 * so the result shows what was added, not what was gained after the
 * 5-coin entry fee. The score only ever goes up (a wrong pick
 * is +0, never -1), so the HUD never shows a negative.
 *
 * Message slot: the band under the buttons (y=308) hosts exactly one
 * line -- the refusal note wins it, otherwise the score line does.
 * The note used to sit at y=280, inside the field; it now shares the
 * score line's slot, which is why it carries the short copy (the chord
 * under the field is only ~213 px at y=325).
 *
 * Controls (user decision 2026-11-08): a tap on one of the three
 * comparison buttons picks that comparison (>, < or =); a right pick
 * scores one point, a wrong pick scores nothing, and either tap
 * deals the next question at once. A press that moves more than
 * ~12 px is a drag and can never pause, so a lingering drag does not
 * stop the game by accident; a press held still for 500 ms on the
 * question area pauses. The hold is tracked by hand, not LVGL's
 * LONG_PRESSED, and the hold layer sits UNDER the answer buttons,
 * so a tap on a button never also pauses. The back arrow stays
 * hidden while playing -- hold first, then leave from the pause
 * card.
 *
 * Power: no sensor is used (unlike 金币雨 / 毛毛跳跃, there is no
 * tilt here), so there is nothing to wake or sleep. A running round
 * pokes the idle timer (muse_state_poke) because a fast player may
 * answer for seconds without the screen sleeping mid-round (same
 * idiom as 金币雨 and video playback).
 */
#include "muse_maketen_ui.h"

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
#include "muse_maketen.h"

/* Comparison button geometry (the mockup v4): 90x64, three in a row
 * at y=210 -- > at x=45, < at x=135, = at x=225. CARD fill with an
 * ACCENT border, like the make-ten v1 number buttons; the symbol is
 * TEXT, centred by lv_obj_center. */
#define MUSE_MAKETEN_BTN_W 90
#define MUSE_MAKETEN_BTN_H 64
#define MUSE_MAKETEN_BTN_Y 210
#define MUSE_MAKETEN_BTN_GT_X 45
#define MUSE_MAKETEN_BTN_LT_X 135
#define MUSE_MAKETEN_BTN_EQ_X 225
/* Question line (the mockup v4): Montserrat 28 at y=96. The digits
 * and + = ? are ASCII, so they do not need the 16 px Noto body font. */
#define MUSE_MAKETEN_EQ_Y 96
/* Hint line (the mockup v4): the 16 px Noto body font, DIM, at
 * y=150 -- under the question, above the answer buttons. It carries
 * CJK ("提示："), so it cannot use a Montserrat size; the mockup's
 * 26 px is not available in the build. */
#define MUSE_MAKETEN_HINT_Y 150
/* Hold to pause: the finger stays within this radius for this long,
 * on the question area only (the doodle page's numbers). */
#define MUSE_MAKETEN_HOLD_MS 500u
#define MUSE_MAKETEN_HOLD_MOVE_PX 12
/* The hold layer covers the question zone, y=60..190: above the
 * answer buttons, so a tap on a button never starts a hold. It
 * covers both the question line (y=96) and the hint line
 * (y=150), so holding either line pauses. */
#define MUSE_MAKETEN_HOLD_Y 60
#define MUSE_MAKETEN_HOLD_H 130

static muse_maketen_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the snake page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* Press tracking for the hold-to-pause (question area only). */
static bool s_press_active = false;
static bool s_press_moved = false;
static uint32_t s_press_start_ms = 0;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;

static lv_obj_t *s_eq;
static lv_obj_t *s_hint;
static lv_obj_t *s_gt_btn;
static lv_obj_t *s_lt_btn;
static lv_obj_t *s_eq_btn;
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
#define MUSE_MAKETEN_HUD_Y 308
#define MUSE_MAKETEN_NOTE_Y 308

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
    /* Question line: "a + b ? d", large. */
    char eq_buf[32];
    snprintf(eq_buf, sizeof(eq_buf), "%d + %d ? %d", s_game.a, s_game.b,
             s_game.d);
    set_label(s_eq, eq_buf);
    /* Hint line: "提示：y+z=x" -- the make-ten split of one addend,
     * smaller and DIM so it reads as a scaffold, not the question. */
    char hint_buf[32];
    snprintf(hint_buf, sizeof(hint_buf), "提示：%d+%d=%d", s_game.hint_y,
             s_game.hint_z, s_game.hint_x);
    set_label(s_hint, hint_buf);
    /* HUD: score + time while playing/paused, gross payout at OVER. */
    char hud_buf[48];
    if (s_game.phase == MUSE_MAKETEN_OVER) {
        snprintf(hud_buf, sizeof(hud_buf), "本局 %u 分，+%u 金币",
                 (unsigned)s_game.score, (unsigned)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud_buf, sizeof(hud_buf), "本局 %u 分 · 剩余 %u 秒",
                 (unsigned)s_game.score,
                 (unsigned)muse_maketen_remaining_s(&s_game));
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud_buf);
    /* Title: the game name, or "游戏结束" at OVER. */
    set_label(s_name, s_game.phase == MUSE_MAKETEN_OVER ? "游戏结束"
                                                             : "凑十法练习");
    /* Overlays: exactly one visible at a time. */
    if (s_game.phase == MUSE_MAKETEN_START) {
        lv_obj_remove_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else if (s_game.phase == MUSE_MAKETEN_PAUSED) {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else if (s_game.phase == MUSE_MAKETEN_OVER) {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Answer buttons and the question lines only take input while playing;
     * the hold layer only while playing too. The back arrow is hidden
     * while playing, as on snake. */
    bool playing = (s_game.phase == MUSE_MAKETEN_PLAYING);
    if (playing) {
        lv_obj_remove_flag(s_hold, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_hold, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_opa(s_gt_btn, playing ? LV_OPA_COVER : LV_OPA_50,
                         LV_PART_MAIN);
    lv_obj_set_style_opa(s_lt_btn, playing ? LV_OPA_COVER : LV_OPA_50,
                         LV_PART_MAIN);
    lv_obj_set_style_opa(s_eq_btn, playing ? LV_OPA_COVER : LV_OPA_50,
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
        /* Short copy on purpose (see MUSE_MAKETEN_NOTE_Y): the slot
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
        muse_maketen_restart(&s_game);
    } else {
        muse_maketen_start(&s_game);
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
    muse_maketen_resume(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void pause_game(void)
{
    muse_maketen_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 10 the same day). muse_pet_round_coins() is the
 * only conversion curve -- 1 point per correct pick, 10 coins
 * per round ceiling -- and the 5-coin entry fee was already charged
 * at start through PET_FEE, so the round's net is payout minus 5,
 * shown on the OVER card. The score only ever goes up (a wrong
 * pick is +0), so no floor helper is needed -- round_coins()
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
    if (s_game.phase != MUSE_MAKETEN_PLAYING) {
        return;
    }
    /* A running round is activity: a fast player may answer for
     * seconds without the screen sleeping mid-round (same idiom as
     * 金币雨 and video playback). */
    muse_state_poke();
    /* Hold to pause: the finger stayed within ~12 px for 500 ms on
     * the question area. A drag that lingers never reaches this branch
     * (s_press_moved). */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms >= MUSE_MAKETEN_HOLD_MS) {
        s_press_active = false;
        pause_game();
        return;
    }
    muse_maketen_tick(&s_game, MUSE_MAKETEN_TICK_MS);
    if (s_game.phase == MUSE_MAKETEN_OVER) {
        settle_round();
        lv_timer_pause(s_timer);
    }
    refresh_display();
}

/* --- comparison buttons --------------------------------------------- */

static void on_cmp(lv_event_t *e, muse_maketen_cmp_t cmp)
{
    if (s_game.phase != MUSE_MAKETEN_PLAYING) {
        return;
    }
    muse_maketen_answer(&s_game, cmp);
    refresh_display();
}

static void on_gt(lv_event_t *e)
{
    (void)e;
    on_cmp(e, MUSE_MAKETEN_CMP_GT);
}

static void on_lt(lv_event_t *e)
{
    (void)e;
    on_cmp(e, MUSE_MAKETEN_CMP_LT);
}

static void on_eq(lv_event_t *e)
{
    (void)e;
    on_cmp(e, MUSE_MAKETEN_CMP_EQ);
}

/* --- hold to pause (question area only) --------------------------- */

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
    if (abs(p.x - s_press_x) > MUSE_MAKETEN_HOLD_MOVE_PX ||
        abs(p.y - s_press_y) > MUSE_MAKETEN_HOLD_MOVE_PX) {
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
        /* Undimmed START overlay: transparent, so the question stays
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

/* One comparison button: a 90x64 CARD fill with an ACCENT border and
 * a TEXT symbol (>, < or =) in Montserrat 28, centred by
 * lv_obj_center -- the same centring the mockup uses (ink-box
 * centred on both axes). */
static lv_obj_t *cmp_btn(lv_obj_t *page, lv_coord_t x, const char *symbol,
                         lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(page);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, MUSE_MAKETEN_BTN_W, MUSE_MAKETEN_BTN_H);
    lv_obj_set_pos(btn, x, MUSE_MAKETEN_BTN_Y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(MUSE_THEME_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(MUSE_THEME_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sym = label(btn, &lv_font_montserrat_28, MUSE_THEME_TEXT);
    lv_label_set_text(sym, symbol);
    lv_obj_center(sym);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void muse_maketen_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_maketen_init(&s_game);

    /* Question line: Montserrat 28, centred at y=96 (the mockup v4).
     * Created before the buttons so the buttons sit on top of it. */
    s_eq = label(page, &lv_font_montserrat_28, MUSE_THEME_TEXT);
    lv_obj_align(s_eq, LV_ALIGN_TOP_MID, 0, MUSE_MAKETEN_EQ_Y);

    /* Hint line: the 16 px Noto body font, DIM, centred at y=150
     * (the mockup v4) -- the make-ten split, a scaffold not the
     * question. */
    s_hint = label(page, muse_font_cjk(), MUSE_THEME_DIM);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, MUSE_MAKETEN_HINT_Y);

    /* Comparison buttons: > < = in that order (the mockup v4). */
    s_gt_btn = cmp_btn(page, MUSE_MAKETEN_BTN_GT_X, ">", on_gt);
    s_lt_btn = cmp_btn(page, MUSE_MAKETEN_BTN_LT_X, "<", on_lt);
    s_eq_btn = cmp_btn(page, MUSE_MAKETEN_BTN_EQ_X, "=", on_eq);

    /* Hold-to-pause layer: transparent, over the question zone only
     * (y=60..190), UNDER the answer buttons -- it is created after the
     * buttons would put it on top of them, so it goes here and the
     * buttons are re-raised above it. A tap on a button never starts
     * a hold. */
    s_hold = fill(page, MUSE_THEME_BG);
    lv_obj_set_style_bg_opa(s_hold, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(s_hold, 360, MUSE_MAKETEN_HOLD_H);
    lv_obj_set_pos(s_hold, 0, MUSE_MAKETEN_HOLD_Y);
    lv_obj_add_flag(s_hold, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_hold, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_hold, on_hold_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_hold, on_hold_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_hold, on_hold_released, LV_EVENT_RELEASED, NULL);
    lv_obj_move_foreground(s_gt_btn);
    lv_obj_move_foreground(s_lt_btn);
    lv_obj_move_foreground(s_eq_btn);

    /* START overlay: not dimmed, the question stays visible behind it. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = centre_btn(s_start_ov, 131, 147);
    lv_obj_t *start_lb = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(start_lb, "开始");
    lv_obj_center(start_lb);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    /* Hint line (mockup v3): same slot as the snake page's hint. */
    lv_obj_t *hint_lb = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint_lb, "算出左边，再比大小");
    lv_obj_align(hint_lb, LV_ALIGN_TOP_MID, 0, 304);

    /* PAUSED overlay: dim, ACCENT title, 继续 button with the PLAY symbol
     * above the label (snake's card). No restart capsule: it charged 5
     * coins on a single tap in the narrowest part of the bezel; the back
     * arrow leaves, and 开始 charges again on the next visit. */
    s_paused_ov = overlay(page, true);
    lv_obj_t *paused_lb = label(s_paused_ov, muse_font_cjk(), MUSE_THEME_ACCENT);
    lv_label_set_text(paused_lb, "已暂停");
    lv_obj_align(paused_lb, LV_ALIGN_TOP_MID, 0, 100);
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
     * (MUSE_MAKETEN_HUD_Y = 308, snake's score slot): page-level,
     * above the dim overlays, so they stay readable in every state.
     * refresh_display() swaps the title to "游戏结束" at OVER. */
    s_name = muse_page_title(page, "凑十法练习", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_MAKETEN_HUD_Y);

    /* Refusal note: below the buttons, sharing the score line's slot
     * (user request 2026-11-01). */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_MAKETEN_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_MAKETEN_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_maketen_ui_show(void)
{
    refresh_display();
}

void muse_maketen_ui_hide(void)
{
    if (s_game.phase == MUSE_MAKETEN_PLAYING) {
        muse_maketen_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    s_press_active = false;
    refresh_display();
}

void muse_maketen_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_MAKETEN_PLAYING ||
        s_game.phase == MUSE_MAKETEN_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_maketen_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}

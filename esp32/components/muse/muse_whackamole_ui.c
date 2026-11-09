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
 * The top crescent shows the title (y=38: the same slot and ACCENT colour
 * the snake page uses for its name): "打地鼠" in every state except OVER,
 * where the slot itself becomes "游戏结束" (UI polish 2026-11-01, same as
 * snake). The line under the field (y=308, the slot the snake page
 * prints its score on) never rides on the bezel: while PLAYING or PAUSED
 * it is white and reads "本局 n 分 · 剩余 X 秒" -- the score plus the
 * 30 s round's countdown (muse_whackamole_remaining_s(), 30 at start,
 * 0 at the end), no coin conversion mid-round -- and at OVER it turns
 * GOLD and reads "本局 n 分，+m 金币" with m the gross payout. There is
 * no pause
 * button any more (2026-11-01, user request "参考贪吃蛇"): the old 34 px one
 * sat at (283,9), i.e. 195 px from the centre on a 180 px-radius panel, so
 * the crescent clipped most of it. Pausing is a LONG PRESS on the field,
 * exactly like the snake page's centre-button hold, and the back arrow (which is
 * hidden while playing, as on snake) becomes reachable through it.
 * A press that actually hit a mole never pauses: the hit is the deliberate
 * action, and turning a solid tap into a pause would read as a punishment.
 * A transparent full-page touch layer, visible only while playing,
 * maps presses to hole cells; outside the hole grid a press does
 * nothing. The START overlay is NOT dimmed (the field stays visible
 * behind it); the PAUSED and OVER overlays are 50% black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the mini
 * games): the PAUSED card has an ACCENT title at y=100 and a 98x66
 * ACCENT-bordered centre button, and the OVER card has the price
 * "- 10 金币" on its own line under the replay label -- on_restart charges
 * the fee the moment it is pressed, so the price belongs on the button.
 * The OVER card itself carries no title and no payout line any more (UI
 * polish 2026-11-01): both moved to the page-level title and HUD slots.
 * The PAUSED card carries no restart capsule: it charged 10 coins on a
 * single tap in the narrowest part of the bezel, and leaving is the back
 * arrow's job (same decision as snake).
 *
 * Payout (UI polish 2026-11-01, superseding the economy branch's net
 * line): at OVER the HUD turns gold and reads "本局 n 分，+m 金币" with m
 * the GROSS payout the wallet actually accepted (s_round_coins, from
 * muse_pet_round_coins() -- 1 point per coin, 20 coins per round ceiling
 * from the economy branch; the score is settled once, at OVER). The net
 * is deliberately NOT printed: the user said kids cannot tell net apart,
 * so the result shows what was added, not what was gained after the
 * 10-coin entry fee. 30 s is one round, one point is one mole.
 *
 * Message slot (user request 2026-11-01, "提示统一显示在游戏区域下方"): the
 * band under the field (y=308) hosts exactly one line -- the refusal note
 * wins it, otherwise the score line does. The note used to sit at y=280,
 * inside the field on top of the second row of holes; it now shares the
 * score line's slot, which is why it carries the short copy (the chord
 * under the field is only ~213 px at y=325).
 */
#include "muse_whackamole_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_audio.h"   /* game sound effects */
#include "muse_font.h"
#include "muse_widgets.h"
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
/* True when the press currently being tracked already hit a mole: such a
 * press must not turn into a pause (see on_touch_hold). */
static bool s_press_hit = false;
/* Round payout bookkeeping, same shape as the snake page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;

static lv_obj_t *s_mole_img[MUSE_WHACKAMOLE_HOLES];
static lv_obj_t *s_lip_img[MUSE_WHACKAMOLE_HOLES];
static lv_obj_t *s_touch;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_paused_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_back;
static lv_obj_t *s_name;
static lv_obj_t *s_hud;
static lv_obj_t *s_note;
static lv_timer_t *s_timer;

/* Status / message slot under the field (field ends at y=300): the HUD
 * and the refusal note share it, one at a time, so both are below the
 * play area (user request 2026-11-01). ~213 px of chord at the line's
 * bottom (325) and the longest string here ("本局 n 分 · 剩余 X 秒",
 * ~180 px) still clears the bezel. */
#define MUSE_WHACKAMOLE_HUD_Y 308
#define MUSE_WHACKAMOLE_NOTE_Y 308

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

/* The band under the field holds one line: the refusal note wins it, the
 * score line takes it otherwise (and stays hidden on START, where there is
 * no round to report). Without this the note and the score printed on top
 * of each other at y=308. */
static void sync_message_band(void)
{
    bool note_up = s_note && !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    if (s_hud) {
        if (note_up || s_game.phase == MUSE_WHACKAMOLE_START) {
            lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* lv_label_set_text has no same-text short circuit in LVGL 9.5; the HUD is
 * rewritten every 100 ms tick, so skip when nothing changed (same trick as
 * muse_menu's set_text). */
static void set_label(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void show_note(const char *text)
{
    lv_label_set_text(s_note, text);
    lv_obj_clear_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

static void hide_note(void)
{
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
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
    muse_whackamole_phase_t ph = s_game.phase;
    /* Title: "打地鼠" except at OVER, where the title slot itself
     * becomes the result title (UI polish 2026-11-01, same as snake),
     * and at PAUSED, where it becomes "已暂停" (user 2026-11-08: the
     * pause title moves to the title slot on every game, so the pause
     * card is just the 继续 button). */
    set_label(s_name, ph == MUSE_WHACKAMOLE_OVER ? "游戏结束"
                     : ph == MUSE_WHACKAMOLE_PAUSED ? "已暂停" : "打地鼠");
    /* HUD, same slot as the snake page's score line: while PLAYING or
     * PAUSED it is white, score plus the 30 s countdown, no coin
     * conversion mid-round (user request 2026-11-01: the countdown that
     * used to lead this line is back, the conversion is not). At OVER
     * it turns GOLD and adds the gross payout the wallet actually
     * accepted; the net is deliberately not printed (user: kids cannot
     * tell net apart). A refused settlement leaves s_round_coins at 0,
     * so the line honestly reads +0. */
    char hud[48];
    if (ph == MUSE_WHACKAMOLE_OVER) {
        snprintf(hud, sizeof(hud), "本局 %lu 分，+%lu 金币",
                 (unsigned long)s_game.score, (unsigned long)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud, sizeof(hud), "本局 %lu 分 · 剩余 %lu 秒",
                 (unsigned long)s_game.score,
                 (unsigned long)muse_whackamole_remaining_s(&s_game));
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud);
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
    /* Touch only while playing. The back arrow is visible on every overlay
     * and hidden while playing (snake's rule): leaving mid-game means
     * long-pressing the field to pause first, so a stray tap on the rim can
     * never abandon a round. */
    if (ph == MUSE_WHACKAMOLE_PLAYING) {
        lv_obj_clear_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
    /* Which of the two lines under the field is the one on screen. */
    sync_message_band();
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
        /* Short copy on purpose (see MUSE_WHACKAMOLE_NOTE_Y): the slot under
         * the field leaves ~213 px of chord and the long sentence is ~272 px
         * wide. Same wording as the sudoku page's note. */
        char note[32];
        snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
                 (unsigned)MUSE_PET_GAME_STAKE);
        show_note(note);
        /* Refused at the door: the low sound. */
        muse_audio_sfx(MUSE_SFX_BAD);
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
    /* Charged: the round starts with the start chirp (also on 再来一局,
     * which comes through here with restart=true). */
    muse_audio_sfx(MUSE_SFX_START);
    seed_once();
    /* A new round: its payout has not been settled yet. */
    s_settled = false;
    s_round_coins = 0;
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

/* Long press = pause (user request 2026-11-01, "参考贪吃蛇"). LVGL's long
 * press fires at LV_INDEV_DEF_LONG_PRESS_TIME (400 ms) and that value is
 * global, shared with other pages, so this page does not change it. A press
 * that hit a mole does NOT pause: the hit already scored, and turning a
 * slightly-held tap into a pause would feel like a penalty. Moles occupy at
 * most one of six holes, so an empty area is always within reach. */
static void on_touch_hold(lv_event_t *e)
{
    (void)e;
    if (s_game.phase != MUSE_WHACKAMOLE_PLAYING || s_press_hit) {
        return;
    }
    on_pause(e);
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 20 on 2026-11-08).
 * muse_pet_round_coins() is the only conversion curve -- 1 point (one mole)
 * per coin, 20 coins per round ceiling -- and the 10-coin entry fee was
 * already charged at start through PET_FEE, so the round's net is payout
 * minus 10, computed on the OVER card, never printed (kids cannot tell
 * net apart: the card shows what was added, not what was gained).
 * A refused request (worker not ready / queue full) leaves s_round_coins
 * at 0 so the card never claims coins the wallet did not get. */
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
    /* Settlement sound: coins in means the round was won (WIN fanfare),
     * 0 coins means 平. Keyed on what was actually credited, so the
     * sound matches the "+m 金币" the OVER card shows. */
    muse_audio_sfx(s_round_coins > 0 ? MUSE_SFX_WIN : MUSE_SFX_DRAW);
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
        /* The 30 s round is over: convert once, here and nowhere else. */
        settle_round();
    }
    refresh_display();
}

static void on_touch(lv_event_t *e)
{
    (void)e;
    /* Every new press starts as "did not hit" so a hit from the previous
     * round can never block the next pause. */
    s_press_hit = false;
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
        s_press_hit = true;
        /* A hit is a +1 moment. A miss stays silent (rapid taps). */
        muse_audio_sfx(MUSE_SFX_GOOD);
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
        lv_obj_set_style_bg_color(ov, lv_color_hex(MUSE_THEME_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ov, LV_OPA_50, LV_PART_MAIN);
    } else {
        /* Undimmed START overlay: transparent, so the field stays
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

void muse_whackamole_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_whackamole_init(&s_game);

    /* No CARD field behind the holes (game-polish 2026-11-07): the holes
     * sit directly on the black page. Per-hole stacks: hole -> mole -> lip. */
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
    s_touch = fill(page, MUSE_THEME_BG);
    lv_obj_set_style_bg_opa(s_touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(s_touch, 360, 360);
    lv_obj_set_pos(s_touch, 0, 0);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_touch, on_touch, LV_EVENT_PRESSED, NULL);
    /* Pause is a long press on the field, the snake page's gesture (user
     * request 2026-11-01). It replaces the 34 px pause button that used to
     * sit at (283,9): its centre was 195 px from the round panel's centre,
     * i.e. outside the 180 px bezel, so only a clipped sliver of it was
     * ever touchable. */
    lv_obj_add_event_cb(s_touch, on_touch_hold, LV_EVENT_LONG_PRESSED, NULL);

    /* START overlay: not dimmed, the field stays visible behind it. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = centre_btn(s_start_ov, 131, 147);
    lv_obj_t *start_lb = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(start_lb, "开始");
    lv_obj_center(start_lb);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);

    /* PAUSED overlay: dim, 继续 button with the PLAY symbol above
     * the label (snake's card). No "已暂停" label on the card any
     * more: the page title itself becomes "已暂停" while paused
     * (user 2026-11-08). No restart capsule: it charged 10 coins on
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

    /* OVER overlay: dim + 再玩一次 button only (UI polish 2026-11-01).
     * The result title lives in the page-title slot and the result line
     * under the field -- both page-level, above the dim -- so the overlay
     * itself carries no title and no payout line any more. No hits/missed
     * split here either. */
    s_over_ov = overlay(page, true);
    lv_obj_t *again_btn = centre_btn(s_over_ov, 131, 147);
    lv_obj_t *again_lb = label(again_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(again_lb, "再玩一次");
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

    /* Pause button removed (2026-11-01): it sat outside the bezel -- see the
     * touch layer above, where the long-press handler took over its job. */

    /* Title (top crescent, y=38: snake's name slot, shared subpage title
     * font/colour) and the result line right under the field
     * (MUSE_WHACKAMOLE_HUD_Y = 308, snake's score slot): page-level, above
     * the dim overlays, so they stay readable in every state. The line used
     * to sit at y=332 and rode on the bezel (user report 2026-11-01).
     * refresh_display() swaps the title to "游戏结束" at OVER. */
    s_name = muse_page_title(page, "打地鼠", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_WHACKAMOLE_HUD_Y);

    /* Refusal note: below the field, sharing the score line's slot (user
     * request 2026-11-01). It used to sit at y=280, inside the field on top
     * of the second row of holes. */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_WHACKAMOLE_NOTE_Y);
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
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}

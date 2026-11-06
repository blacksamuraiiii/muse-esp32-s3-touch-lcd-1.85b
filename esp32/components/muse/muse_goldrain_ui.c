/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_goldrain_ui.c -- 金币雨 subpage, see muse_goldrain_ui.h.
 *
 * Layout (mockup v9, 360x360 round screen, dark theme like the other
 * games): a 240x240 field at (60,60). Buttercup (53x66) stands on the
 * field's bottom edge; coins (18 px) and bombs fall from the top. The
 * top crescent shows the title (y=38: the same slot and ACCENT colour
 * the snake page uses for its name): "金币雨" in every state except
 * OVER, where the slot itself becomes "游戏结束" (same as snake /
 * whack-a-mole). The line under the field (y=308, the slot the snake
 * page prints its score on) never rides on the bezel: while PLAYING or
 * PAUSED it is white and reads "本局 n 分 · 剩余 X 秒" -- the score
 * plus the 30 s round's countdown, no coin conversion mid-round -- and
 * at OVER it turns GOLD and reads "本局 n 分，+m 金币" with m the gross
 * payout. There is no pause button any more (2026-11-01, user request
 * "参考贪吃蛇"): the old 34 px one sat at (283,9), i.e. 195 px from
 * the centre on a 180 px-radius panel, so the crescent clipped most of
 * it. Pausing is a LONG HOLD on the field (finger within ~12 px for
 * 500 ms), and the back arrow (which is hidden while playing, as on
 * snake) becomes reachable through it. The START overlay is NOT dimmed
 * (the field stays visible behind it); the PAUSED and OVER overlays
 * are 50% black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the
 * mini games): the PAUSED card has an ACCENT title at y=100 and a
 * 98x66 ACCENT-bordered centre button, and the OVER card has the price
 * "- 5 金币" on its own line under the replay label -- on_restart
 * charges the fee the moment it is pressed, so the price belongs on
 * the button. The OVER card itself carries no title and no payout line
 * any more (UI polish 2026-11-01): both moved to the page-level title
 * and HUD slots. The PAUSED card carries no restart capsule: it
 * charged 5 coins on a single tap in the narrowest part of the bezel,
 * and leaving is the back arrow's job (same decision as snake). The
 * START overlay carries the hint line at y=304 (DIM, same slot as the
 * snake page's hint): "左右倾斜移动，长按中间暂停".
 *
 * Payout (same as snake / whack-a-mole): at OVER the HUD turns gold
 * and reads "本局 n 分，+m 金币" with m the GROSS payout the wallet
 * actually accepted (s_round_coins, from muse_pet_round_coins() --
 * 1 point per coin, 10 coins per round ceiling; the score is settled
 * once, at OVER). The net is deliberately NOT printed: the user said
 * kids cannot tell net apart, so the result shows what was added, not
 * what was gained after the 5-coin entry fee. 30 s is one round, one
 * point is one coin. Scoring (user rule 2026-11-06): a coin is +1, a
 * bomb is -5, so the score can go below zero mid-round; the settlement
 * floors it at zero (muse_goldrain_settle_score), so a round pays at
 * most 10 coins and at least 0 -- never a negative amount.
 *
 * Message slot: the band under the field (y=308) hosts exactly one
 * line -- the refusal note wins it, otherwise the score line does.
 * The note used to sit at y=280, inside the field; it now shares the
 * score line's slot, which is why it carries the short copy (the chord
 * under the field is only ~213 px at y=325).
 *
 * Controls: tilt (primary) + finger drag (backup) + hold to pause.
 * The tilt reads the accelerometer once per 50 ms tick through
 * muse_imu.c (official waveshare/qmi8658 component, polling only).
 * Which chip axis is left-right, and which sign runs which way,
 * depends on the chip's mounting orientation on this board, so both
 * are calibration knobs below (MUSE_GOLDRAIN_TILT_SIGN and the gain):
 * if the catcher runs the wrong way on the real board, flip the sign.
 * The drag moves her directly under the finger; a press that moves
 * more than ~12 px is a drag and can never pause, so a lingering drag
 * does not stop the game by accident.
 */
#include "muse_goldrain_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_font.h"
#include "muse_widgets.h"
#include "muse_pet.h"        /* snapshot + request: the entry fee */
#include "muse_pet_model.h"
#include "muse_theme.h"
#include "muse_goldrain.h"
#include "muse_goldrain_art.h"
#include "muse_imu.h"
#include "muse_state.h"     /* poke: no auto-sleep while a round runs */

/* Tilt calibration (user decision 2026-11-06: accelerometer, official
 * module, coins not too fast). The board is held screen-up, facing
 * the player; rolling it left/right moves the gravity vector along
 * the chip's X axis. Gain: a 35-degree tilt is ~5.6 m/s^2 on that
 * axis and should move her nearly across the 240 px field
 * (40 px per m/s^2 * 5.6 = 224 px). Dead zone: ~3 degrees, so a
 * hand-held board that is not perfectly level does not drift her.
 * Sign: -1 since the 1.6b on-device check -- the user reported that
 * left-edge-down (right side up) ran her right, so the chip's X axis
 * on this board points the other way than the mockup assumed. Flip
 * back to +1 only if a future board mounts the chip rotated.
 * Speed: she eases toward the tilt target at MUSE_GOLDRAIN_TILT_MAX_PX_S
 * (1.6a jumped straight there in one 50 ms tick, which read as a
 * teleport); a finger drag is still 1:1 under the finger. */
#define MUSE_GOLDRAIN_TILT_GAIN_PX_PER_MS2 40.0f
#define MUSE_GOLDRAIN_TILT_DEADZONE_MS2 0.5f
#define MUSE_GOLDRAIN_TILT_SMOOTH 0.35f
#define MUSE_GOLDRAIN_TILT_SIGN -1.0f
#define MUSE_GOLDRAIN_TILT_MAX_PX_S 130.0f
/* Hold to pause: the finger stays within this radius for this long. */
#define MUSE_GOLDRAIN_HOLD_MS 500u
#define MUSE_GOLDRAIN_HOLD_MOVE_PX 12

static muse_goldrain_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the snake page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* Tilt state: the filtered accel x and the tilt-driven target. The drag
 * writes s_tilt_cx too, so the tilt does not yank her back on release. */
static float s_ax_filt = 0.0f;
static float s_tilt_cx = 180.0f;
/* Press tracking for the drag and the hold-to-pause. */
static bool s_press_active = false;
static bool s_press_moved = false;
static uint32_t s_press_start_ms = 0;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;

static lv_obj_t *s_coin_img[MUSE_GOLDRAIN_MAX_ITEMS];
static lv_obj_t *s_bomb[MUSE_GOLDRAIN_MAX_ITEMS];
static lv_obj_t *s_sprite;
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
#define MUSE_GOLDRAIN_HUD_Y 308
#define MUSE_GOLDRAIN_NOTE_Y 308

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
        if (note_up || s_game.phase == MUSE_GOLDRAIN_START) {
            lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* lv_label_set_text has no same-text short circuit in LVGL 9.5; the HUD is
 * rewritten every 50 ms tick, so skip when nothing changed (same trick as
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
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        muse_goldrain_item_t *it = &s_game.items[i];
        if (!it->active) {
            lv_obj_add_flag(s_coin_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bomb[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_t *show = it->kind == MUSE_GOLDRAIN_COIN ? s_coin_img[i] : s_bomb[i];
        lv_obj_t *hide = it->kind == MUSE_GOLDRAIN_COIN ? s_bomb[i] : s_coin_img[i];
        lv_obj_set_pos(show, (lv_coord_t)it->x, (lv_coord_t)it->y);
        lv_obj_clear_flag(show, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hide, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(s_sprite,
                   (lv_coord_t)(s_game.catcher_cx - MUSE_GOLDRAIN_SPRITE_W / 2),
                   MUSE_GOLDRAIN_CATCH_Y);
    muse_goldrain_phase_t ph = s_game.phase;
    /* Title: "金币雨" except at OVER, where the title slot itself
     * becomes the result title (same as snake / whack-a-mole). */
    set_label(s_name, ph == MUSE_GOLDRAIN_OVER ? "游戏结束" : "金币雨");
    /* HUD, same slot as the snake page's score line: while PLAYING or
     * PAUSED it is white, score plus the 30 s countdown, no coin
     * conversion mid-round. At OVER it turns GOLD and adds the gross
     * payout the wallet actually accepted; the net is deliberately not
     * printed (user: kids cannot tell net apart). A refused settlement
     * leaves s_round_coins at 0, so the line honestly reads +0. */
    char hud[48];
    if (ph == MUSE_GOLDRAIN_OVER) {
        snprintf(hud, sizeof(hud), "本局 %ld 分，+%lu 金币",
                 (long)s_game.score, (unsigned long)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud, sizeof(hud), "本局 %ld 分 · 剩余 %lu 秒",
                 (long)s_game.score,
                 (unsigned long)muse_goldrain_remaining_s(&s_game));
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud);
    if (ph == MUSE_GOLDRAIN_START) {
        lv_obj_clear_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_GOLDRAIN_PAUSED) {
        lv_obj_clear_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_GOLDRAIN_OVER) {
        lv_obj_clear_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Touch only while playing. The back arrow is visible on every overlay
     * and hidden while playing (snake's rule): leaving mid-game means
     * holding the field to pause first, so a stray tap on the rim can
     * never abandon a round. */
    if (ph == MUSE_GOLDRAIN_PLAYING) {
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
        /* Short copy on purpose (see MUSE_GOLDRAIN_NOTE_Y): the slot under
         * the field leaves ~213 px of chord and the long sentence is ~272 px
         * wide. Same wording as the sudoku page's note. */
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
    s_ax_filt = 0.0f;
    s_tilt_cx = (float)(MUSE_GOLDRAIN_FIELD_X + MUSE_GOLDRAIN_FIELD_SIZE / 2);
    if (restart) {
        muse_goldrain_restart(&s_game);
    } else {
        muse_goldrain_start(&s_game);
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
    muse_goldrain_resume(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void pause_game(void)
{
    muse_goldrain_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

static void on_pause(lv_event_t *e)
{
    (void)e;
    pause_game();
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 10 the same day; floor added 2026-11-06).
 * muse_pet_round_coins() is the only conversion curve -- 1 point (one
 * coin) per coin, 10 coins per round ceiling -- and the 5-coin entry
 * fee was already charged at start through PET_FEE, so the round's net
 * is payout minus 5, shown on the OVER card. The score is floored at
 * zero first (muse_goldrain_settle_score): a bomb is -5, so a round
 * can end below zero, and that pays 0 coins, never a negative amount
 * -- round_coins() takes an unsigned score, so an unfloored negative
 * would wrap into a huge payout. A refused request (worker not ready /
 * queue full) leaves s_round_coins at 0 so the card never claims coins
 * the wallet did not get. */
static void settle_round(void)
{
    if (s_settled) {
        return;
    }
    s_settled = true;
    uint32_t coins =
        muse_pet_round_coins(muse_goldrain_settle_score(s_game.score));
    s_round_coins = 0;
    if (coins != 0 && muse_pet_request_play_reward(coins)) {
        s_round_coins = coins;
    }
}

/* --- tilt (primary control) --------------------------------------- */

/* One accel read per tick: low-pass the roll axis, dead zone, then map
 * to a catcher target. Inside the dead zone she stays put -- holding
 * the board level must not drag her back to the centre. A failed read
 * is ignored for this tick (she keeps her last target). */
static void update_tilt(void)
{
    if (!muse_imu_available()) {
        return;
    }
    float ax, ay, az;
    if (muse_imu_read_accel(&ax, &ay, &az) != ESP_OK) {
        return;
    }
    s_ax_filt += MUSE_GOLDRAIN_TILT_SMOOTH * (ax - s_ax_filt);
    if (s_ax_filt < -MUSE_GOLDRAIN_TILT_DEADZONE_MS2 ||
        s_ax_filt > MUSE_GOLDRAIN_TILT_DEADZONE_MS2) {
        s_tilt_cx = (float)(MUSE_GOLDRAIN_FIELD_X + MUSE_GOLDRAIN_FIELD_SIZE / 2) +
                    MUSE_GOLDRAIN_TILT_SIGN * MUSE_GOLDRAIN_TILT_GAIN_PX_PER_MS2 *
                        s_ax_filt;
        /* Glide, don't teleport (1.6b): one tick is 50 ms, so a raw jump
         * to the target crosses the whole field between two frames. */
        const float step = MUSE_GOLDRAIN_TILT_MAX_PX_S *
                           (float)MUSE_GOLDRAIN_TICK_MS / 1000.0f;
        float next = s_tilt_cx;
        if (next > s_game.catcher_cx + step) {
            next = s_game.catcher_cx + step;
        } else if (next < s_game.catcher_cx - step) {
            next = s_game.catcher_cx - step;
        }
        muse_goldrain_set_catcher(&s_game, next);
    }
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_GOLDRAIN_PLAYING) {
        return;
    }
    /* A running round is activity: playing by tilt alone never touches
     * the screen, so without this the idle timer slept the screen
     * mid-round (1.6b; same idiom as video playback and a voice turn). */
    muse_state_poke();
    /* Hold to pause: the finger stayed within ~12 px for 500 ms. A
     * drag that lingers never reaches this branch (s_press_moved). */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms >= MUSE_GOLDRAIN_HOLD_MS) {
        s_press_active = false;
        pause_game();
        return;
    }
    update_tilt();
    muse_goldrain_tick(&s_game, MUSE_GOLDRAIN_TICK_MS);
    if (s_game.phase == MUSE_GOLDRAIN_OVER) {
        lv_timer_pause(s_timer);
        /* The 30 s round is over: convert once, here and nowhere else. */
        settle_round();
    }
    refresh_display();
}

/* --- touch (backup control: drag) --------------------------------- */

static void on_touch_pressed(lv_event_t *e)
{
    (void)e;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    s_press_active = true;
    s_press_moved = false;
    s_press_start_ms = lv_tick_get();
    s_press_x = point.x;
    s_press_y = point.y;
    if (s_game.phase == MUSE_GOLDRAIN_PLAYING) {
        /* A tap jumps her under the finger; a drag keeps moving her. */
        s_tilt_cx = (float)point.x;
        muse_goldrain_set_catcher(&s_game, (float)point.x);
    }
}

static void on_touch_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_press_active || s_game.phase != MUSE_GOLDRAIN_PLAYING) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int dx = point.x - s_press_x;
    int dy = point.y - s_press_y;
    if (dx * dx + dy * dy >
        MUSE_GOLDRAIN_HOLD_MOVE_PX * MUSE_GOLDRAIN_HOLD_MOVE_PX) {
        s_press_moved = true;
    }
    s_tilt_cx = (float)point.x;
    muse_goldrain_set_catcher(&s_game, (float)point.x);
}

static void on_touch_released(lv_event_t *e)
{
    (void)e;
    s_press_active = false;
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

/* The bomb is drawn, not an asset: a black circle with a short fuse
 * and a gold spark, 18x18 like the coin. Built once per item slot and
 * shown only when that slot holds a bomb. */
static lv_obj_t *bomb_obj(lv_obj_t *page)
{
    lv_obj_t *bomb = lv_obj_create(page);
    lv_obj_remove_style_all(bomb);
    lv_obj_set_size(bomb, MUSE_GOLDRAIN_COIN_PX, MUSE_GOLDRAIN_COIN_PX);
    lv_obj_remove_flag(bomb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bomb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bomb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *body = lv_obj_create(bomb);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 14, 14);
    lv_obj_set_pos(body, 2, 4);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    /* Half the size: a full circle. (LV_RADIUS_CIRCLE is not used: the
     * snake page already moved off it, and a literal cannot go stale.) */
    lv_obj_set_style_radius(body, 7, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *fuse = lv_obj_create(bomb);
    lv_obj_remove_style_all(fuse);
    lv_obj_set_size(fuse, 2, 4);
    lv_obj_set_pos(fuse, 8, 1);
    lv_obj_set_style_bg_color(fuse, lv_color_hex(MUSE_THEME_DIM), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fuse, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(fuse, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fuse, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *spark = lv_obj_create(bomb);
    lv_obj_remove_style_all(spark);
    lv_obj_set_size(spark, 4, 4);
    lv_obj_set_pos(spark, 7, 0);
    lv_obj_set_style_bg_color(spark, lv_color_hex(MUSE_THEME_GOLD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(spark, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(spark, 2, LV_PART_MAIN);
    lv_obj_remove_flag(spark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spark, LV_OBJ_FLAG_CLICKABLE);
    return bomb;
}

void muse_goldrain_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_goldrain_init(&s_game);

    /* Field, then one coin image and one bomb drawing per item slot. */
    lv_obj_t *field = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(field, MUSE_GOLDRAIN_FIELD_SIZE,
                    MUSE_GOLDRAIN_FIELD_SIZE);
    lv_obj_set_pos(field, MUSE_GOLDRAIN_FIELD_X, MUSE_GOLDRAIN_FIELD_Y);
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        lv_obj_t *coin = lv_image_create(page);
        lv_image_set_src(coin, &muse_goldrain_coin);
        lv_obj_remove_flag(coin, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(coin, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(coin, LV_OBJ_FLAG_HIDDEN);
        s_coin_img[i] = coin;
        s_bomb[i] = bomb_obj(page);
    }

    /* Buttercup, standing on the field's bottom edge. */
    s_sprite = lv_image_create(page);
    lv_image_set_src(s_sprite, &muse_goldrain_sprite);
    lv_obj_set_pos(s_sprite,
                   (lv_coord_t)(s_game.catcher_cx - MUSE_GOLDRAIN_SPRITE_W / 2),
                   MUSE_GOLDRAIN_CATCH_Y);
    lv_obj_remove_flag(s_sprite, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_sprite, LV_OBJ_FLAG_CLICKABLE);

    /* Touch layer: full page, only while playing, above the field.
     * fill() strips CLICKABLE, so it is added back: without it the
     * layer would never receive PRESSED events. */
    s_touch = fill(page, MUSE_THEME_BG);
    lv_obj_set_style_bg_opa(s_touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(s_touch, 360, 360);
    lv_obj_set_pos(s_touch, 0, 0);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_touch, on_touch_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_touch, on_touch_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_touch, on_touch_released, LV_EVENT_RELEASED, NULL);

    /* START overlay: not dimmed, the field stays visible behind it. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = centre_btn(s_start_ov, 131, 147);
    lv_obj_t *start_lb = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(start_lb, "开始");
    lv_obj_center(start_lb);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    /* Hint line (mockup v9): same slot as the snake page's hint. */
    lv_obj_t *hint_lb = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint_lb, "左右倾斜移动，长按中间暂停");
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
     * under the field -- both page-level, above the dim -- so the overlay
     * itself carries no title and no payout line any more. No hits/missed
     * split here either. */
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
     * font/colour) and the result line right under the field
     * (MUSE_GOLDRAIN_HUD_Y = 308, snake's score slot): page-level, above
     * the dim overlays, so they stay readable in every state. The line used
     * to sit at y=332 and rode on the bezel (user report 2026-11-01).
     * refresh_display() swaps the title to "游戏结束" at OVER. */
    s_name = muse_page_title(page, "金币雨", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_GOLDRAIN_HUD_Y);

    /* Refusal note: below the field, sharing the score line's slot (user
     * request 2026-11-01). It used to sit at y=280, inside the field on top
     * of the second row of holes. */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_GOLDRAIN_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_GOLDRAIN_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_goldrain_ui_show(void)
{
    muse_imu_wake();
    refresh_display();
}

void muse_goldrain_ui_hide(void)
{
    if (s_game.phase == MUSE_GOLDRAIN_PLAYING) {
        muse_goldrain_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    s_press_active = false;
    muse_imu_sleep();
    refresh_display();
}

void muse_goldrain_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_GOLDRAIN_PLAYING ||
        s_game.phase == MUSE_GOLDRAIN_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_goldrain_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}

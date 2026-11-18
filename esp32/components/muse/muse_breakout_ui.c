/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_breakout_ui.c -- 打砖块 subpage, see muse_breakout_ui.h.
 *
 * Layout (mockup v4/v5, 360x360 round screen, dark theme like the other
 * games): a 240x240 field at (60,60), bordered, black inside. 24 bricks
 * (6x4, 36x16) in four coloured rows, a 72x10 paddle on the bottom edge
 * and one white ball. The top crescent shows the title (y=38: the same
 * slot and ACCENT colour the snake page uses for its name): "打砖块" in
 * every state except OVER, where the slot itself becomes "游戏结束"
 * (same as snake / whack-a-mole / goldrain / doodle). The line under the
 * field (y=308, the slot the snake page prints its score on) never rides
 * on the bezel: while PLAYING or PAUSED it is white and reads
 * "本局 n 分" -- score only, no countdown (the round ends on bricks or
 * lives, not on a clock) -- and at OVER it turns GOLD and reads
 * "本局 n 分，+m 金币" with m the gross payout. There is no pause button
 * any more (2026-11-01, user request "参考贪吃蛇"): the old 34 px one
 * sat at (283,9), i.e. 195 px from the centre on a 180 px-radius panel,
 * so the crescent clipped most of it. Pausing is a LONG HOLD on the
 * field (finger within ~12 px for 500 ms), and the back arrow (which is
 * hidden while playing, as on snake) becomes reachable through it. The
 * START overlay is NOT dimmed (the field stays visible behind it); the
 * PAUSED and OVER overlays are 50% black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the
 * mini games): the PAUSED card is a 98x66 ACCENT-bordered centre
 * button ("继续", PLAY symbol above the label; the "已暂停" title
 * lives in the page-title slot, not on the card), and the OVER card
 * has the price
 * "- 10 金币" on its own line under the replay label -- on_restart
 * charges the fee the moment it is pressed, so the price belongs on
 * the button. The OVER card itself carries no title and no payout line
 * any more (UI polish 2026-11-01): both moved to the page-level title
 * and HUD slots. The PAUSED card carries no restart capsule: it
 * charged 10 coins on a single tap in the narrowest part of the bezel,
 * and leaving is the back arrow's job (same decision as snake). The
 * START overlay carries the hint line at y=304 (DIM, same slot as the
 * snake page's hint): "左右倾斜移动挡板".
 *
 * Payout (same as snake / whack-a-mole / goldrain / doodle): at OVER the
 * HUD turns gold and reads "本局 n 分，+m 金币" with m the GROSS payout
 * the wallet actually accepted (s_round_coins, from
 * muse_pet_round_coins() -- 1 point per coin, 20 coins per round
 * ceiling; the score is settled once, at OVER). The net is deliberately
 * NOT printed: the user said kids cannot tell net apart, so the result
 * shows what was added, not what was gained after the 10-coin entry
 * fee. A full board is 24 points and pays 20 (net +10); the break-even
 * is 10 points, like the other games. The score only ever goes up
 * (one brick is +1, there is no penalty item), so no floor helper is
 * needed -- round_coins() takes an unsigned score.
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
 * are calibration knobs in muse_breakout.h
 * (MUSE_BREAKOUT_TILT_SIGN and the gain): if the paddle runs the
 * wrong way on the real board, flip the sign. The drag moves the
 * paddle directly under the finger; a press that moves more than ~12 px
 * is a drag and can never pause, so a lingering drag does not stop the
 * game by accident.
 *
 * Sounds (v4 only, no new sounds): a broken brick is GOOD, a paddle
 * catch is PLACE (the shortest sound -- catches are the most frequent),
 * a lost life is BAD, start / replay is START, settlement is WIN (or
 * DRAW when 0 coins were credited), a refused fee is BAD, wall and
 * brick-edge bounces are silent.
 */
#include "muse_breakout_ui.h"

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
#include "muse_breakout.h"
#include "muse_imu.h"
#include "muse_state.h"     /* poke: no auto-sleep while a round runs */

/* Hold to pause: the finger stays within this radius for this long. */
#define MUSE_BREAKOUT_HOLD_MS 500u
#define MUSE_BREAKOUT_HOLD_MOVE_PX 12

/* Brick row colours, top to bottom: red / yellow / green / purple
 * (the mockup v4's rows). The highlight strip on each brick is the
 * same colour lightened by 60 per channel, clamped at 255 -- the
 * mockup's 4 px top strip, drawn as a child of the brick. */
static const uint32_t s_row_colors[MUSE_BREAKOUT_BRICK_ROWS] = {
    MUSE_THEME_DANGER, MUSE_THEME_GOLD, MUSE_THEME_OK, MUSE_THEME_ACCENT,
};
static const uint32_t s_row_hi[MUSE_BREAKOUT_BRICK_ROWS] = {
    0xff9898, 0xffff87, 0xabfffb, 0xe3b9ff,
};
/* Paddle highlight: the ACCENT paddle lightened the same way. */
#define MUSE_BREAKOUT_PADDLE_HI_HEX 0xe3b9ff

static muse_breakout_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the snake page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* Tilt state: the filtered accel x and the tilt-driven target. The drag
 * writes s_tilt_cx too, so the tilt does not yank the paddle back on
 * release. */
static float s_ax_filt = 0.0f;
static float s_tilt_cx = 180.0f;
/* Press tracking for the drag and the hold-to-pause. */
static bool s_press_active = false;
static bool s_press_moved = false;
static uint32_t s_press_start_ms = 0;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;

static lv_obj_t *s_brick[MUSE_BREAKOUT_BRICK_COUNT];
static lv_obj_t *s_ball;
static lv_obj_t *s_paddle;
static lv_obj_t *s_touch;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_paused_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_back;
static lv_obj_t *s_name;
static lv_obj_t *s_hud;
static lv_obj_t *s_note;
static lv_obj_t *s_page;
static lv_timer_t *s_timer;

/* Status / message slot under the field (field ends at y=300): the HUD
 * and the refusal note share it, one at a time, so both are below the
 * play area (user request 2026-11-01). ~213 px of chord at the line's
 * bottom (325) and the longest string here ("本局 n 分，+m 金币",
 * ~180 px) still clears the bezel. */
#define MUSE_BREAKOUT_HUD_Y 308
#define MUSE_BREAKOUT_NOTE_Y 308

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
        if (note_up || s_game.phase == MUSE_BREAKOUT_START) {
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
    for (int row = 0; row < MUSE_BREAKOUT_BRICK_ROWS; row++) {
        for (int col = 0; col < MUSE_BREAKOUT_BRICK_COLS; col++) {
            lv_obj_t *brick =
                s_brick[row * MUSE_BREAKOUT_BRICK_COLS + col];
            if (muse_breakout_brick_alive(&s_game, col, row)) {
                lv_obj_clear_flag(brick, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(brick, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    lv_obj_set_pos(s_ball,
                   (lv_coord_t)(s_game.ball_x - MUSE_BREAKOUT_BALL_R),
                   (lv_coord_t)(s_game.ball_y - MUSE_BREAKOUT_BALL_R));
    lv_obj_set_pos(s_paddle,
                   (lv_coord_t)(s_game.paddle_cx -
                                MUSE_BREAKOUT_PADDLE_W / 2),
                   MUSE_BREAKOUT_PADDLE_Y);
    muse_breakout_phase_t ph = s_game.phase;
    /* Title: "打砖块" except at OVER, where the title slot itself
     * becomes the result title, and at PAUSED, where it becomes
     * "已暂停" (user 2026-11-08: the pause title moves to the title
     * slot on every game, so the pause card is just the 继续 button). */
    set_label(s_name, ph == MUSE_BREAKOUT_OVER ? "游戏结束"
                     : ph == MUSE_BREAKOUT_PAUSED ? "已暂停" : "打砖块");
    /* HUD, same slot as the snake page's score line: while PLAYING or
     * PAUSED it is white, score only, no coin conversion mid-round.
     * At OVER it turns GOLD and adds the gross payout the wallet
     * actually accepted; the net is deliberately not printed (user:
     * kids cannot tell net apart). A refused settlement leaves
     * s_round_coins at 0, so the line honestly reads +0. */
    char hud[48];
    if (ph == MUSE_BREAKOUT_OVER) {
        snprintf(hud, sizeof(hud), "本局 %ld 分，+%lu 金币",
                 (long)s_game.score, (unsigned long)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud, sizeof(hud), "本局 %ld 分", (long)s_game.score);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud);
    if (ph == MUSE_BREAKOUT_START) {
        lv_obj_clear_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_BREAKOUT_PAUSED) {
        lv_obj_clear_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_BREAKOUT_OVER) {
        lv_obj_clear_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Touch only while playing. The back arrow is visible on every overlay
     * and hidden while playing (snake's rule): leaving mid-game means
     * holding the field to pause first, so a stray tap on the rim can
     * never abandon a round. */
    if (ph == MUSE_BREAKOUT_PLAYING) {
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
        /* Short copy on purpose (see MUSE_BREAKOUT_NOTE_Y): the slot under
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
    /* Tilt is the primary control: wake the accelerometer (no-op on a
     * board without the chip) and start the low-pass clean. */
    muse_imu_wake();
    s_ax_filt = 0.0f;
    s_tilt_cx = (float)(MUSE_BREAKOUT_FIELD_X + MUSE_BREAKOUT_FIELD_SIZE / 2);
    if (restart) {
        muse_breakout_restart(&s_game);
    } else {
        muse_breakout_start(&s_game);
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
    muse_breakout_resume(&s_game);
    /* The sensor may have been slept while the pause card was up (leaving
     * the page, or the interact tile being hidden, calls
     * muse_breakout_ui_hide). Waking here is idempotent, and 继续 is the one
     * action that restarts the ticks, so tilt must be armed by now. */
    muse_imu_wake();
    lv_timer_resume(s_timer);
    refresh_display();
}

static void pause_game(void)
{
    muse_breakout_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 20 on 2026-11-08). muse_pet_round_coins() is the
 * only conversion curve -- 1 point (one brick) per coin, 20 coins per
 * round ceiling -- and the 10-coin entry fee was already charged at
 * start through PET_FEE, so the round's net is payout minus 10, computed
 * on the OVER card, never printed (kids cannot tell net apart: the card
 * shows what was added, not what was gained). The score can never go
 * negative (bricks give +1, there is no penalty item), so no floor
 * helper is needed -- round_coins() takes an unsigned score. A refused
 * request (worker not ready / queue full) leaves s_round_coins at 0 so
 * the card never claims coins the wallet did not get. */
static void settle_round(void)
{
    if (s_settled) {
        return;
    }
    s_settled = true;
    uint32_t coins = muse_pet_round_coins((uint32_t)s_game.score);
    s_round_coins = 0;
    if (coins != 0 && muse_pet_request_play_reward(coins)) {
        s_round_coins = coins;
    }
    /* Settlement sound: coins in means the round was won (WIN fanfare),
     * 0 coins means 平. Keyed on what was actually credited, so the
     * sound matches the "+m 金币" the OVER card shows. */
    muse_audio_sfx(s_round_coins > 0 ? MUSE_SFX_WIN : MUSE_SFX_DRAW);
}

/* --- tilt (primary control) --------------------------------------- */

/* One accel read per tick: low-pass the roll axis, dead zone, then map
 * to a paddle target and glide toward it at the 260 px/s cap. Inside
 * the dead zone the paddle stays put -- holding the board level must
 * not drag it back to the centre. A failed read is ignored for this
 * tick (it keeps its last target). The pure pieces (target, dead zone,
 * glide) live in muse_breakout.c so the host harness can check them. */
static void update_tilt(void)
{
    if (!muse_imu_available()) {
        return;
    }
    float ax, ay, az;
    if (muse_imu_read_accel(&ax, &ay, &az) != ESP_OK) {
        return;
    }
    s_ax_filt += MUSE_BREAKOUT_TILT_SMOOTH * (ax - s_ax_filt);
    if (muse_breakout_tilt_active(s_ax_filt)) {
        s_tilt_cx = muse_breakout_tilt_target(s_ax_filt);
        float next = muse_breakout_glide(s_game.paddle_cx, s_tilt_cx,
                                         MUSE_BREAKOUT_TICK_MS);
        muse_breakout_set_paddle(&s_game, next);
    }
}

/* --- floating "+1" ------------------------------------------------ */
/* The effect itself is muse_float_text() in muse_widgets.c: 金币雨 shows
 * the same +1 (user 2026-11-07 asked for it there too), so it lives in the
 * shared widgets instead of a per-page copy. */

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_BREAKOUT_PLAYING) {
        return;
    }
    /* A running round is activity: playing by tilt alone never touches
     * the screen, so without this the idle timer slept the screen
     * mid-round (same idiom as video playback and a voice turn). */
    muse_state_poke();
    /* Hold to pause: the finger stayed within ~12 px for 500 ms. A
     * drag that lingers never reaches this branch (s_press_moved). */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms >= MUSE_BREAKOUT_HOLD_MS) {
        s_press_active = false;
        pause_game();
        return;
    }
    update_tilt();
    muse_breakout_tick(&s_game, MUSE_BREAKOUT_TICK_MS);
    if (s_game.event == MUSE_BREAKOUT_EV_BRICK) {
        /* Broke a brick: +1, in gold, centred on the brick's empty
         * slot (the logic layer reports the brick's centre). */
        muse_float_text(s_page, "+1", lv_color_hex(MUSE_THEME_GOLD),
                        (int)s_game.event_x, (int)s_game.event_y);
        muse_audio_sfx(MUSE_SFX_GOOD);
    } else if (s_game.event == MUSE_BREAKOUT_EV_PADDLE) {
        /* Caught on the paddle: the shortest sound (catches are the
         * most frequent event; the brick sound stays the score one). */
        muse_audio_sfx(MUSE_SFX_PLACE);
    } else if (s_game.event == MUSE_BREAKOUT_EV_LOST) {
        /* Lost a life: the low sound. */
        muse_audio_sfx(MUSE_SFX_BAD);
    }
    if (s_game.phase == MUSE_BREAKOUT_OVER) {
        lv_timer_pause(s_timer);
        /* Bricks gone or lives gone: convert once, here and nowhere else. */
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
    if (s_game.phase == MUSE_BREAKOUT_PLAYING) {
        /* A tap jumps the paddle under the finger; a drag keeps moving
         * it. The drag writes s_tilt_cx too, so the tilt does not yank
         * it back on release. */
        s_tilt_cx = (float)point.x;
        muse_breakout_set_paddle(&s_game, (float)point.x);
    }
}

static void on_touch_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_press_active || s_game.phase != MUSE_BREAKOUT_PLAYING) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int dx = point.x - s_press_x;
    int dy = point.y - s_press_y;
    if (dx * dx + dy * dy >
        MUSE_BREAKOUT_HOLD_MOVE_PX * MUSE_BREAKOUT_HOLD_MOVE_PX) {
        s_press_moved = true;
    }
    /* Drag is 1:1 under the finger -- the 260 px/s cap only applies
     * to the tilt glide, never to a finger. */
    s_tilt_cx = (float)point.x;
    muse_breakout_set_paddle(&s_game, (float)point.x);
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

/* One brick: a 36x16 rounded rect in its row colour, with a 4 px
 * highlight strip across the top (the mockup v4's lit edge). The strip
 * is a child of the brick, so hiding a broken brick hides both. */
static lv_obj_t *brick_obj(lv_obj_t *page, int col, int row)
{
    float x, y;
    muse_breakout_brick_pos(col, row, &x, &y);
    lv_obj_t *brick = lv_obj_create(page);
    lv_obj_remove_style_all(brick);
    lv_obj_set_size(brick, MUSE_BREAKOUT_BRICK_W, MUSE_BREAKOUT_BRICK_H);
    lv_obj_set_pos(brick, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_style_bg_color(brick, lv_color_hex(s_row_colors[row]),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(brick, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(brick, 3, LV_PART_MAIN);
    lv_obj_remove_flag(brick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(brick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *hi = lv_obj_create(brick);
    lv_obj_remove_style_all(hi);
    lv_obj_set_size(hi, MUSE_BREAKOUT_BRICK_W, 4);
    lv_obj_set_pos(hi, 0, 0);
    lv_obj_set_style_bg_color(hi, lv_color_hex(s_row_hi[row]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hi, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(hi, 2, LV_PART_MAIN);
    lv_obj_remove_flag(hi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hi, LV_OBJ_FLAG_CLICKABLE);
    return brick;
}

/* The paddle: a 72x10 ACCENT rounded rect with a 3 px highlight strip
 * across the top (the mockup v4's lit edge). */
static lv_obj_t *paddle_obj(lv_obj_t *page)
{
    lv_obj_t *paddle = lv_obj_create(page);
    lv_obj_remove_style_all(paddle);
    lv_obj_set_size(paddle, MUSE_BREAKOUT_PADDLE_W, MUSE_BREAKOUT_PADDLE_H);
    lv_obj_set_pos(paddle,
                   (lv_coord_t)(s_game.paddle_cx -
                                MUSE_BREAKOUT_PADDLE_W / 2),
                   MUSE_BREAKOUT_PADDLE_Y);
    lv_obj_set_style_bg_color(paddle, lv_color_hex(MUSE_THEME_ACCENT),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(paddle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(paddle, 5, LV_PART_MAIN);
    lv_obj_remove_flag(paddle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(paddle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *hi = lv_obj_create(paddle);
    lv_obj_remove_style_all(hi);
    lv_obj_set_size(hi, MUSE_BREAKOUT_PADDLE_W, 3);
    lv_obj_set_pos(hi, 0, 0);
    lv_obj_set_style_bg_color(hi, lv_color_hex(MUSE_BREAKOUT_PADDLE_HI_HEX),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hi, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(hi, 2, LV_PART_MAIN);
    lv_obj_remove_flag(hi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hi, LV_OBJ_FLAG_CLICKABLE);
    return paddle;
}

void muse_breakout_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_breakout_init(&s_game);
    s_page = page;

    /* Field: border only (2 px DIM, radius 6), black inside -- no CARD
     * bottom under the bricks, unlike the snake board. The theme has no
     * BORDER colour of its own; DIM is the muted outline the other
     * pages use for lines that must not fight the play pieces. */
    lv_obj_t *field = lv_obj_create(page);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, MUSE_BREAKOUT_FIELD_SIZE,
                    MUSE_BREAKOUT_FIELD_SIZE);
    lv_obj_set_pos(field, MUSE_BREAKOUT_FIELD_X, MUSE_BREAKOUT_FIELD_Y);
    /* No frame around the play area (user, 2026-11-18: the 2 px border made
     * the field look small): the bricks, paddle and ball sit straight on the
     * page background, which also reads as the same field the mockup has. */
    lv_obj_set_style_bg_opa(field, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(field, 0, LV_PART_MAIN);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_CLICKABLE);

    for (int row = 0; row < MUSE_BREAKOUT_BRICK_ROWS; row++) {
        for (int col = 0; col < MUSE_BREAKOUT_BRICK_COLS; col++) {
            s_brick[row * MUSE_BREAKOUT_BRICK_COLS + col] =
                brick_obj(page, col, row);
        }
    }

    /* The ball: a single white 10x10 circle, repositioned every tick --
     * no trail, no motion lines. */
    s_ball = lv_obj_create(page);
    lv_obj_remove_style_all(s_ball);
    lv_obj_set_size(s_ball, MUSE_BREAKOUT_BALL_R * 2,
                    MUSE_BREAKOUT_BALL_R * 2);
    lv_obj_set_pos(s_ball,
                   (lv_coord_t)(s_game.ball_x - MUSE_BREAKOUT_BALL_R),
                   (lv_coord_t)(s_game.ball_y - MUSE_BREAKOUT_BALL_R));
    lv_obj_set_style_bg_color(s_ball, lv_color_hex(MUSE_THEME_TEXT),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ball, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ball, MUSE_BREAKOUT_BALL_R, LV_PART_MAIN);
    lv_obj_remove_flag(s_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ball, LV_OBJ_FLAG_CLICKABLE);

    s_paddle = paddle_obj(page);

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
    /* Hint line (mockup v6): same slot as the snake page's hint. The
     * 8-character line reads 128 px in the 16 px CJK face, well inside
     * the chord at y=304..320 (261..226 px) -- no bezel clipping to
     * check (user 2026-11-18: drop the ", 长按中间暂停" half). */
    lv_obj_t *hint_lb = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint_lb, "左右倾斜移动挡板");
    lv_obj_align(hint_lb, LV_ALIGN_TOP_MID, 0, 304);

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
     * (MUSE_BREAKOUT_HUD_Y = 308, snake's score slot): page-level, above
     * the dim overlays, so they stay readable in every state. The line used
     * to sit at y=332 and rode on the bezel (user report 2026-11-01).
     * refresh_display() swaps the title to "游戏结束" at OVER. */
    s_name = muse_page_title(page, "打砖块", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_BREAKOUT_HUD_Y);

    /* Refusal note: below the field, sharing the score line's slot (user
     * request 2026-11-01). It used to sit at y=280, inside the field on top
     * of the second row of holes. */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_BREAKOUT_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_BREAKOUT_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_breakout_ui_show(void)
{
    muse_imu_wake();
    refresh_display();
}

void muse_breakout_ui_hide(void)
{
    if (s_game.phase == MUSE_BREAKOUT_PLAYING) {
        muse_breakout_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    s_press_active = false;
    muse_imu_sleep();
    refresh_display();
}

void muse_breakout_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_BREAKOUT_PLAYING ||
        s_game.phase == MUSE_BREAKOUT_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_breakout_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}

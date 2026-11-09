/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_doodle_ui.c -- 毛毛跳跃 subpage, see muse_doodle_ui.h.
 *
 * Layout (mockup v4, 360x360 round screen, dark theme like the other
 * games): a 240x240 field at (60,60) with a night sky (faint stars,
 * no grid). Buttercup (53x66) hops -- tilted a little into her
 * direction, no motion lines, nothing under her feet after a bounce --
 * and clouds (66x26, puffy: a base plus three circles) are the only
 * footing. Coins (18 px, the goldrain token) float in the air in a
 * rising trail and are the only score (+1 each); there is no bomb
 * (user 2026-11-07: "取消炸弹惩罚").
 * The top crescent shows the title (y=38: the same slot and
 * ACCENT colour the snake page uses for its name): "毛毛跳跃" in every
 * state except OVER, where the slot itself becomes "游戏结束" (same
 * as snake / whack-a-mole / goldrain). The line under the field
 * (y=308, the slot the snake page prints its score on) never rides
 * on the bezel: while PLAYING or PAUSED it is white and reads
 * "本局 n 分 · 剩余 s 秒" (score plus the 60 s countdown, same as
 * 金币雨's 30 s line), and at OVER it turns GOLD and reads
 * "本局 n 分，+m 金币" with m the gross payout. There is no pause
 * button any more (2026-11-01, user request "参考贪吃蛇"): the old
 * 34 px one sat at (283,9), i.e. 195 px from the centre on a 180 px
 * radius panel, so the crescent clipped most of it. Pausing is a
 * LONG HOLD on the field (finger within ~12 px for 500 ms), and the
 * back arrow (which is hidden while playing, as on snake) becomes
 * reachable through it. The START overlay is NOT dimmed (the field
 * stays visible behind it); the PAUSED and OVER overlays are 50%
 * black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the
 * mini games): the PAUSED card has an ACCENT title at y=100 and a
 * 98x66 ACCENT-bordered centre button, and the OVER card has the
 * price "- 10 金币" on its own line under the replay label -- on_restart
 * charges the fee the moment it is pressed, so the price belongs on
 * the button. The OVER card itself carries no title and no payout line
 * any more (UI polish 2026-11-01): both moved to the page-level title
 * and HUD slots. The PAUSED card carries no restart capsule: it
 * charged 10 coins on a single tap in the narrowest part of the bezel,
 * and leaving is the back arrow's job (same decision as snake). The
 * START overlay carries the hint line at y=304 (DIM, same slot as the
 * snake page's hint): "倾斜左右移动，长按中间暂停".
 *
 * Payout (same as snake / whack-a-mole / goldrain): at OVER the HUD
 * turns gold and reads "本局 n 分，+m 金币" with m the GROSS payout
 * the wallet actually accepted (s_round_coins, from
 * muse_pet_round_coins() -- 1 point per coin, 20 coins per round
 * ceiling; the score is settled once, at OVER). The net is
 * deliberately NOT printed: the user said kids cannot tell net apart,
 * so the result shows what was added, not what was gained after the
 * 10-coin entry fee. Clouds give no points and coins are +1, so the
 * score only ever goes up and the HUD never shows a negative.
 *
 * Message slot: the band under the field (y=308) hosts exactly one
 * line -- the refusal note wins it, otherwise the score line does.
 * The note used to sit at y=280, inside the field; it now shares the
 * score line's slot, which is why it carries the short copy (the chord
 * under the field is only ~213 px at y=325).
 *
 * Controls (user decision 2026-11-07, switched from tap-to-fly to
 * classic Doodle Jump): TILT is primary -- the QMI8658 roll (the same
 * sensor and low-pass the 金币雨 page uses) sets the horizontal
 * direction, and the board is level = she keeps her line. A tap on the
 * left/right half of the field adds a shove that decays over ~0.4 s
 * (about 70 px), so a kid who has not learned the tilt can still steer;
 * the two sum, they never fight (muse_doodle.c). There is no jump
 * button: clouds bounce her. A press that moves more than ~12 px is a
 * drag and can never pause, so a lingering drag does not stop the game
 * by accident; a press held still for 500 ms pauses. The hold is
 * tracked by hand, not LVGL's LONG_PRESSED, and the tap shove fires on
 * RELEASE only when the press was a quick tap (under 500 ms, unmoved)
 * -- a hold that already paused never also shoves her.
 *
 * Power: the accelerometer is woken when a round starts
 * (muse_imu_wake) and put back to sleep when the page is left
 * (muse_imu_sleep), the same discipline as the 金币雨 page. A running
 * round pokes the idle timer (muse_state_poke) because tilt-only play
 * never touches the screen and the display would otherwise sleep
 * mid-round.
 */
#include "muse_doodle_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_audio.h"   /* game sound effects */
#include "muse_font.h"
#include "muse_widgets.h"
#include "muse_pet.h"        /* snapshot + request: the entry fee */
#include "muse_pet_model.h"
#include "muse_state.h"
#include "muse_theme.h"
#include "muse_doodle.h"
#include "muse_goldrain_art.h"   /* the shared 18 px coin */
#include "muse_doodle_art.h"     /* our own 53x66 sprite */
#include "muse_imu.h"        /* tilt: the board's QMI8658 roll */

/* Cloud colours (the mockup v4): white static, light-blue moving,
 * grey breakable. */
#define MUSE_DOODLE_CLOUD_STATIC_HEX 0xf2efff
#define MUSE_DOODLE_CLOUD_MOVE_HEX 0xb4c8ff
#define MUSE_DOODLE_CLOUD_BREAK_HEX 0x8c88a0
/* Hold to pause: the finger stays within this radius for this long. */
#define MUSE_DOODLE_HOLD_MS 500u
#define MUSE_DOODLE_HOLD_MOVE_PX 12
/* Sprite tilt: she leans into her movement direction, this many
 * 0.1-degree units per px/tick of horizontal speed (6 px/tick is
 * about 18 degrees, capped at 150 = 15 degrees). */
#define MUSE_DOODLE_TILT_PER_VX 30
/* Steering tilt (the 金币雨 page's numbers): one accel read per tick,
 * low-pass the roll axis, dead zone, then map to a -1..1 direction.
 * The sign is the one 金币雨 settled on after its "倾斜反向" fix. */
#define MUSE_DOODLE_STEER_SMOOTH 0.35f
#define MUSE_DOODLE_STEER_DEADZONE_MS2 0.5f
#define MUSE_DOODLE_STEER_FULL_MS2 4.0f
#define MUSE_DOODLE_STEER_SIGN -1.0f

static muse_doodle_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the snake page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* Press tracking for the tap shove and the hold-to-pause. */
static bool s_press_active = false;
static bool s_press_moved = false;
static uint32_t s_press_start_ms = 0;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;
/* Low-passed accel roll (m/s^2), one read per tick while playing. */
static float s_ax_filt = 0.0f;

static lv_obj_t *s_cloud[MUSE_DOODLE_MAX_CLOUDS];
/* Each cloud's puff pieces, so refresh_display can recolour them by
 * kind (white static, blue moving, grey breakable). Index 0 is the
 * base, 1..3 are the three circles. */
static lv_obj_t *s_cloud_piece[MUSE_DOODLE_MAX_CLOUDS][4];
static lv_obj_t *s_coin_img[MUSE_DOODLE_MAX_COINS];
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
 * bottom (325) and the longest string here ("本局 n 分，+m 金币",
 * ~180 px) still clears the bezel. */
#define MUSE_DOODLE_HUD_Y 308
#define MUSE_DOODLE_NOTE_Y 308

/* The night sky: faint stars inside the field, fixed positions so the
 * mockup and the board agree. */
static const lv_point_t s_stars[] = {
    {80, 80}, {120, 70}, {260, 75}, {280, 120}, {90, 140}, {250, 160},
    {85, 240}, {270, 260}, {150, 250}, {200, 90}, {170, 285}, {110, 110},
    {230, 285}, {140, 200}, {290, 200}, {75, 175},
};

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
        if (note_up || s_game.phase == MUSE_DOODLE_START) {
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

static uint32_t cloud_color(muse_doodle_cloud_kind_t kind)
{
    switch (kind) {
        case MUSE_DOODLE_CLOUD_MOVE:
            return MUSE_DOODLE_CLOUD_MOVE_HEX;
        case MUSE_DOODLE_CLOUD_BREAK:
            return MUSE_DOODLE_CLOUD_BREAK_HEX;
        case MUSE_DOODLE_CLOUD_STATIC:
        default:
            return MUSE_DOODLE_CLOUD_STATIC_HEX;
    }
}

static void refresh_display(void)
{
    for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
        muse_doodle_cloud_t *c = &s_game.clouds[i];
        if (!c->active) {
            lv_obj_add_flag(s_cloud[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(s_cloud[i], (lv_coord_t)c->x, (lv_coord_t)c->y);
        uint32_t color = cloud_color(c->kind);
        for (int p = 0; p < 4; p++) {
            lv_obj_set_style_bg_color(s_cloud_piece[i][p], lv_color_hex(color),
                                      LV_PART_MAIN);
        }
        lv_obj_clear_flag(s_cloud[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
        muse_doodle_coin_t *coin = &s_game.coins[i];
        if (!coin->active) {
            lv_obj_add_flag(s_coin_img[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(s_coin_img[i],
                       (lv_coord_t)(coin->x - MUSE_DOODLE_COIN_PX / 2),
                       (lv_coord_t)(coin->y - MUSE_DOODLE_COIN_PX / 2));
        lv_obj_clear_flag(s_coin_img[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* She leans into her direction (no motion lines, user 2026-11-07);
     * level when she is not moving sideways. */
    int16_t rot = (int16_t)(s_game.vx * (float)MUSE_DOODLE_TILT_PER_VX);
    if (rot > 150) {
        rot = 150;
    } else if (rot < -150) {
        rot = -150;
    }
    lv_image_set_rotation(s_sprite, rot);
    lv_obj_set_pos(s_sprite,
                   (lv_coord_t)(s_game.cx - MUSE_DOODLE_SPRITE_W / 2),
                   (lv_coord_t)(s_game.cy - MUSE_DOODLE_SPRITE_H / 2));
    muse_doodle_phase_t ph = s_game.phase;
    /* Title: "毛毛跳跃" except at OVER, where the title slot itself
     * becomes the result title, and at PAUSED, where it becomes
     * "已暂停" (user 2026-11-08: the pause title moves to the title
     * slot on every game, so the pause card is just the 继续 button). */
    set_label(s_name, ph == MUSE_DOODLE_OVER ? "游戏结束"
                     : ph == MUSE_DOODLE_PAUSED ? "已暂停" : "毛毛跳跃");
    /* HUD, same slot as the snake page's score line: while PLAYING or
     * PAUSED it is white, score plus the 60 s countdown (same as
     * 金币雨's 30 s line), no coin conversion mid-round. At OVER it
     * turns GOLD and adds the gross payout the wallet actually
     * accepted; the net is deliberately not printed (user: kids
     * cannot tell net apart). A refused settlement leaves
     * s_round_coins at 0, so the line honestly reads +0. */
    char hud[48];
    if (ph == MUSE_DOODLE_OVER) {
        snprintf(hud, sizeof(hud), "本局 %lu 分，+%lu 金币",
                 (unsigned long)s_game.score, (unsigned long)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud, sizeof(hud), "本局 %lu 分 · 剩余 %lu 秒",
                 (unsigned long)s_game.score,
                 (unsigned long)muse_doodle_remaining_s(&s_game));
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud);
    if (ph == MUSE_DOODLE_START) {
        lv_obj_clear_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_DOODLE_PAUSED) {
        lv_obj_clear_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_DOODLE_OVER) {
        lv_obj_clear_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Touch only while playing. The back arrow is visible on every overlay
     * and hidden while playing (snake's rule): leaving mid-game means
     * holding the field to pause first, so a stray tap on the rim can
     * never abandon a round. */
    if (ph == MUSE_DOODLE_PLAYING) {
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
        /* Short copy on purpose (see MUSE_DOODLE_NOTE_Y): the slot under
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
    if (restart) {
        muse_doodle_restart(&s_game);
    } else {
        muse_doodle_start(&s_game);
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
    muse_doodle_resume(&s_game);
    /* The sensor may have been slept while the pause card was up (leaving
     * the page, or the interact tile being hidden, calls
     * muse_doodle_ui_hide). Waking here is idempotent, and 继续 is the one
     * action that restarts the ticks, so tilt must be armed by now. */
    muse_imu_wake();
    lv_timer_resume(s_timer);
    refresh_display();
}

static void pause_game(void)
{
    muse_doodle_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 20 on 2026-11-08). muse_pet_round_coins() is the
 * only conversion curve -- 1 point (one coin) per coin, 20 coins per
 * round ceiling -- and the 10-coin entry fee was already charged at
 * start through PET_FEE, so the round's net is payout minus 10, computed
 * on the OVER card, never printed (kids cannot tell net apart: the card
 * shows what was added, not what was gained). The score is the coins
 * collected and can never go negative (clouds give no points, there is
 * no bomb), so no floor
 * helper is needed -- round_coins() takes an unsigned score. A refused
 * request (worker not ready / queue full) leaves s_round_coins at 0 so
 * the card never claims coins the wallet did not get. */
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

/* One accel read per tick: low-pass the roll axis, dead zone, then map
 * to a -1..1 steering direction for muse_doodle_tilt(). Inside the
 * dead zone the tilt is 0 -- holding the board level must not drag her
 * sideways. A failed read is ignored for this tick (she keeps her last
 * direction). Boards without the chip simply never call in: the tap
 * shove alone still steers her. */
static void update_tilt(void)
{
    if (!muse_imu_available()) {
        return;
    }
    float ax, ay, az;
    if (muse_imu_read_accel(&ax, &ay, &az) != ESP_OK) {
        return;
    }
    s_ax_filt += MUSE_DOODLE_STEER_SMOOTH * (ax - s_ax_filt);
    float tilt = 0.0f;
    if (s_ax_filt < -MUSE_DOODLE_STEER_DEADZONE_MS2 ||
        s_ax_filt > MUSE_DOODLE_STEER_DEADZONE_MS2) {
        tilt = MUSE_DOODLE_STEER_SIGN * s_ax_filt / MUSE_DOODLE_STEER_FULL_MS2;
    }
    muse_doodle_tilt(&s_game, tilt);
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_DOODLE_PLAYING) {
        return;
    }
    /* A running round is activity: playing by tilt alone never touches
     * the screen, so without this the idle timer slept the screen
     * mid-round (same idiom as 金币雨 and video playback). */
    muse_state_poke();
    /* Hold to pause: the finger stayed within ~12 px for 500 ms. A
     * drag that lingers never reaches this branch (s_press_moved). */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms >= MUSE_DOODLE_HOLD_MS) {
        s_press_active = false;
        pause_game();
        return;
    }
    update_tilt();
    /* Coins are +1 each (user rule 2026-11-07) and there is no bomb, so the
     * score only ever goes up; the delta drives the floating "+1" the user
     * asked for here as well as on 金币雨 (shared muse_float_text()). */
    uint32_t score_before = s_game.score;
    muse_doodle_tick(&s_game, MUSE_DOODLE_TICK_MS);
    uint32_t gained = s_game.score - score_before;
    if (gained > 0 && s_sprite != NULL) {
        char buf[12];
        snprintf(buf, sizeof(buf), "+%u", (unsigned)gained);
        muse_float_text(lv_obj_get_parent(s_sprite), buf,
                        lv_color_hex(MUSE_THEME_GOLD),
                        (int)s_game.cx, (int)s_game.cy - 30);
        /* Caught a coin: the bright sound. (A broken cloud stays
         * silent by design.) */
        muse_audio_sfx(MUSE_SFX_GOOD);
    }
    if (s_game.phase == MUSE_DOODLE_OVER) {
        lv_timer_pause(s_timer);
        /* She fell out of the bottom: convert once, here and nowhere else. */
        settle_round();
    }
    refresh_display();
}

/* --- touch (tap to fly) ------------------------------------------- */

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
}

static void on_touch_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_press_active || s_game.phase != MUSE_DOODLE_PLAYING) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int dx = point.x - s_press_x;
    int dy = point.y - s_press_y;
    if (dx * dx + dy * dy >
        MUSE_DOODLE_HOLD_MOVE_PX * MUSE_DOODLE_HOLD_MOVE_PX) {
        s_press_moved = true;
    }
}

static void on_touch_released(lv_event_t *e)
{
    (void)e;
    /* A quick tap shoves her sideways: the tap's half picks the
     * direction (left half left, right half right). A hold that
     * already paused (or a drag) never shoves on release. */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms < MUSE_DOODLE_HOLD_MS &&
        s_game.phase == MUSE_DOODLE_PLAYING) {
        muse_doodle_tap(&s_game, s_press_x < 180 ? -1 : 1);
        refresh_display();
    }
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

/* One puff piece of a cloud: a plain filled shape, no border, not
 * clickable. Circles use a literal radius (half the size), not
 * LV_RADIUS_CIRCLE (the snake page already moved off it, and a
 * literal cannot go stale). */
static lv_obj_t *puff(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w, lv_coord_t h, lv_coord_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, lv_color_hex(MUSE_DOODLE_CLOUD_STATIC_HEX),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/* A cloud: a transparent 66x26 container holding the base plus three
 * circles (the mockup v4's puffy shape). The container is positioned
 * at the cloud's top-left; refresh_display recolours the pieces by
 * kind. */
static lv_obj_t *cloud_obj(lv_obj_t *page, int idx)
{
    lv_obj_t *cloud = lv_obj_create(page);
    lv_obj_remove_style_all(cloud);
    lv_obj_set_size(cloud, MUSE_DOODLE_CLOUD_W, MUSE_DOODLE_CLOUD_H);
    lv_obj_remove_flag(cloud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cloud, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cloud, LV_OBJ_FLAG_HIDDEN);
    /* Base: 66x12 at y=14, radius 6. Middle puff: 26x26 at (20,0),
     * radius 13. Side puffs: 20x20 at (4,6) and (41,6), radius 10. */
    s_cloud_piece[idx][0] = puff(cloud, 0, 14, 66, 12, 6);
    s_cloud_piece[idx][1] = puff(cloud, 20, 0, 26, 26, 13);
    s_cloud_piece[idx][2] = puff(cloud, 4, 6, 20, 20, 10);
    s_cloud_piece[idx][3] = puff(cloud, 41, 6, 20, 20, 10);
    return cloud;
}

void muse_doodle_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_doodle_init(&s_game);

    /* Field, then one star dot each (fixed positions, the mockup v4's
     * night sky), one cloud container per cloud slot, one coin image
     * per coin slot. */
    lv_obj_t *field = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(field, MUSE_DOODLE_FIELD_SIZE,
                    MUSE_DOODLE_FIELD_SIZE);
    lv_obj_set_pos(field, MUSE_DOODLE_FIELD_X, MUSE_DOODLE_FIELD_Y);
    for (size_t i = 0; i < sizeof(s_stars) / sizeof(s_stars[0]); i++) {
        lv_obj_t *star = lv_obj_create(page);
        lv_obj_remove_style_all(star);
        lv_obj_set_size(star, 2, 2);
        lv_obj_set_pos(star, s_stars[i].x - 1, s_stars[i].y - 1);
        lv_obj_set_style_bg_color(star, lv_color_hex(0x646082), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(star, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(star, 1, LV_PART_MAIN);
        lv_obj_remove_flag(star, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(star, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
        s_cloud[i] = cloud_obj(page, i);
    }
    for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
        lv_obj_t *coin = lv_image_create(page);
        lv_image_set_src(coin, &muse_goldrain_coin);
        lv_obj_remove_flag(coin, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(coin, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(coin, LV_OBJ_FLAG_HIDDEN);
        s_coin_img[i] = coin;
    }
    /* Buttercup, hopping (tilted into her direction by refresh_display). */
    s_sprite = lv_image_create(page);
    lv_image_set_src(s_sprite, &muse_doodle_sprite);
    lv_obj_set_pos(s_sprite,
                   (lv_coord_t)(s_game.cx - MUSE_DOODLE_SPRITE_W / 2),
                   (lv_coord_t)(s_game.cy - MUSE_DOODLE_SPRITE_H / 2));
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
    /* Hint line (mockup v4): same slot as the snake page's hint. */
    lv_obj_t *hint_lb = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint_lb, "倾斜左右移动，长按中间暂停");
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
     * (MUSE_DOODLE_HUD_Y = 308, snake's score slot): page-level, above
     * the dim overlays, so they stay readable in every state. The line used
     * to sit at y=332 and rode on the bezel (user report 2026-11-01).
     * refresh_display() swaps the title to "游戏结束" at OVER. */
    s_name = muse_page_title(page, "毛毛跳跃", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_DOODLE_HUD_Y);

    /* Refusal note: below the field, sharing the score line's slot (user
     * request 2026-11-01). It used to sit at y=280, inside the field on top
     * of the second row of holes. */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_DOODLE_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_DOODLE_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_doodle_ui_show(void)
{
    /* Re-arm the accelerometer on every visit: muse_doodle_ui_hide() put it
     * to sleep, and 金币雨 has the same wake on show (muse_goldrain_ui_show).
     * Without this a pause followed by leaving the page left the sensor off,
     * and 继续 restarted the ticks with tilt dead. */
    muse_imu_wake();
    refresh_display();
}

void muse_doodle_ui_hide(void)
{
    if (s_game.phase == MUSE_DOODLE_PLAYING) {
        muse_doodle_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    s_press_active = false;
    /* Off-screen: the accelerometer goes back to sleep (the 金币雨
     * page's discipline). A return visit wakes it again on 开始. */
    muse_imu_sleep();
    refresh_display();
}

void muse_doodle_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_DOODLE_PLAYING ||
        s_game.phase == MUSE_DOODLE_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_doodle_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}

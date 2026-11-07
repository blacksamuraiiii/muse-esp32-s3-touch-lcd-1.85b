/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_doodle.h -- 毛毛跳跃 logic (no LVGL, no ESP-IDF).
 *
 * Doodle Jump, not flight (user 2026-11-07: "还是从飞行改成跳跃吧，
 * 不然游戏永远结束不了了"). She hops: clouds are footing, and landing
 * on one (falling onto its top) bounces her back up at MUSE_DOODLE_BOUNCE_VY
 * -- there is no tap-to-flap any more, so nothing lets her climb without
 * a cloud under her. Steering is horizontal only: the tilt (muse_imu,
 * the same QMI8658 the 金币雨 page uses) sets the direction and a tap on
 * the left/right half adds a shove in that direction. Clouds are one-way
 * (she passes up through them and only bounces when falling onto them);
 * a grey cloud breaks after one bounce and is replaced at the top at
 * once, so the field never thins out. The game is endless: the camera
 * follows her up, clouds recycle above, and she falls out of the bottom
 * when there is nothing left to bounce on.
 *
 * A round can still be survived without input on a white cloud: it
 * bounces her in place (user decision 2026-11-07, "保留三色"). She dies
 * when the player steers her off the clouds -- that is the round's only
 * end, same as the original game.
 *
 * Score (user rules 2026-11-07): clouds give NO points, a coin is +1,
 * and there is no bomb (user 2026-11-07, "取消炸弹惩罚" while the port
 * was under test), so the score only ever goes up. The settlement is
 * muse_pet_round_coins() directly (1 point per coin, 10 coins per round
 * ceiling). The 5-coin entry fee was already charged at start.
 *
 * Physics is per 50 ms tick (MUSE_DOODLE_TICK_MS), tuned from the
 * classic Doodle Jump references (ImKennyYip's doodle-jump: jump -8 and
 * gravity 0.4 per 60 fps frame, platforms 75 px apart): gravity 0.4
 * px/tick^2, bounce -9 px/tick (about 101 px of climb, one and a half
 * cloud rows). Clouds sit ~70 px apart vertically, so a bounce nearly
 * always reaches the next row. Horizontal speed is 6 px/tick (120 px/s,
 * the same feel as the 金币雨 drag); a tap adds a 6 px/tick shove that
 * decays by 8% a tick (about 75 px of travel), and the field edges wrap
 * around (classic Doodle Jump).
 *
 * Timing is in milliseconds and advances only through
 * muse_doodle_tick(), so the host test drives it deterministically
 * (no lv_tick_get() here). The UI calls tick every 50 ms and pauses
 * its timer while paused, so tick() also no-ops unless PLAYING.
 * Random cloud/coin picks use stdlib rand(), seeded by the caller
 * (the UI seeds once from lv_tick_get on the first start, as the
 * snake page does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_DOODLE_TICK_MS 50u
/* Field geometry, same 240x240 square the other games use. */
#define MUSE_DOODLE_FIELD_X 60
#define MUSE_DOODLE_FIELD_Y 60
#define MUSE_DOODLE_FIELD_SIZE 240
/* Art sizes (muse_goldrain_art.h): the coin is 18 px, Buttercup 53x66. */
#define MUSE_DOODLE_COIN_PX 18
#define MUSE_DOODLE_SPRITE_W 53
#define MUSE_DOODLE_SPRITE_H 66
/* Cloud geometry (the mockup v4): a puffy cloud 66 px wide, 26 px tall. */
#define MUSE_DOODLE_CLOUD_W 66
#define MUSE_DOODLE_CLOUD_H 26
#define MUSE_DOODLE_MAX_CLOUDS 8
#define MUSE_DOODLE_MAX_COINS 8
/* Physics, px per 50 ms tick (see the header comment for the tuning). */
#define MUSE_DOODLE_GRAVITY 0.4f
#define MUSE_DOODLE_BOUNCE_VY (-9.0f)
#define MUSE_DOODLE_MAX_FALL_VY 16.0f
#define MUSE_DOODLE_H_SPEED 6.0f
#define MUSE_DOODLE_MAX_VX 9.0f
/* A tap's shove: starts at this speed and keeps this share each tick. */
#define MUSE_DOODLE_TAP_VX 6.0f
#define MUSE_DOODLE_TAP_DECAY 0.92f
/* Clouds sit this far apart vertically; the camera starts scrolling
 * once her centre rises above this line (field top + 70). */
#define MUSE_DOODLE_CLOUD_SPACING 70
#define MUSE_DOODLE_CAM_Y (MUSE_DOODLE_FIELD_Y + 70)
/* A moving cloud slides this fast, px/tick, bouncing off the field
 * edges. Most new clouds get a coin above them. */
#define MUSE_DOODLE_MOVE_SPEED 1.0f
#define MUSE_DOODLE_COIN_CHANCE_PCT 90u

typedef enum {
    MUSE_DOODLE_START = 0,
    MUSE_DOODLE_PLAYING,
    MUSE_DOODLE_PAUSED,
    MUSE_DOODLE_OVER,
} muse_doodle_phase_t;

typedef enum {
    MUSE_DOODLE_CLOUD_STATIC = 0,
    MUSE_DOODLE_CLOUD_MOVE,
    MUSE_DOODLE_CLOUD_BREAK,
} muse_doodle_cloud_kind_t;

typedef struct {
    bool active;
    muse_doodle_cloud_kind_t kind;
    float x; /* left edge, screen coordinates */
    float y; /* top edge, screen coordinates */
    float vx; /* horizontal speed, px/tick (moving clouds only) */
} muse_doodle_cloud_t;

typedef struct {
    bool active;
    float x; /* centre, screen coordinates */
    float y; /* centre, screen coordinates */
} muse_doodle_coin_t;

typedef struct {
    muse_doodle_phase_t phase;
    muse_doodle_cloud_t clouds[MUSE_DOODLE_MAX_CLOUDS];
    muse_doodle_coin_t coins[MUSE_DOODLE_MAX_COINS];
    float cx; /* her centre x, screen coordinates */
    float cy; /* her centre y, screen coordinates */
    float vx; /* px/tick, this tick's horizontal speed (tilt + shove) */
    float vy; /* px/tick, positive is falling */
    float tilt; /* -1..1, set by muse_doodle_tilt(), kept between ticks */
    float shove; /* px/tick from the last tap, decays every tick */
    uint32_t score; /* +1 per coin; clouds give no points */
    uint32_t caught; /* coins collected */
    uint32_t height_px; /* total px scrolled, i.e. height climbed */
} muse_doodle_t;

void muse_doodle_init(muse_doodle_t *g);

/* START -> PLAYING, resetting the game. No-op from any other phase
 * (use restart for a new game after OVER/PAUSED). */
void muse_doodle_start(muse_doodle_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_doodle_pause(muse_doodle_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_doodle_resume(muse_doodle_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). She is put back
 * STANDING ON the lowest cloud (the bug the user reported 2026-11-07:
 * a restart kept her where she fell, so she was in mid-air with no
 * footing and the next tick killed her again). */
void muse_doodle_restart(muse_doodle_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Applies gravity, moves her, bounces her off clouds, scrolls the
 * camera, recycles clouds above, collects coins, and ends the game
 * when she falls out of the bottom of the field. */
void muse_doodle_tick(muse_doodle_t *g, uint32_t ms);

/* Steering: `tilt` is -1 (full left) .. +1 (full right), normally the
 * smoothed accelerometer roll. Kept until the next call, so a tick
 * always has a direction. No-op unless PLAYING. */
void muse_doodle_tilt(muse_doodle_t *g, float tilt);

/* A tap shoves her sideways: dir < 0 left, dir > 0 right. The shove
 * decays over ~0.4 s (about 75 px) and is added to the tilt, so the
 * two controls never fight. No-op unless PLAYING (a tap on an overlay
 * never shoves her). */
void muse_doodle_tap(muse_doodle_t *g, int dir);

/* Clouds currently on screen (active ones). */
int muse_doodle_cloud_count(const muse_doodle_t *g);

/* Coins currently on screen (active ones). */
int muse_doodle_coin_count(const muse_doodle_t *g);

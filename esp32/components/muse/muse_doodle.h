/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_doodle.h -- 毛毛飞行 logic (no LVGL, no ESP-IDF).
 *
 * Buttercup flies: she does not stand on anything (user 2026-11-07,
 * "毛毛是会飞的，不需要脚底下那个飞行板"). A tap flaps her upward
 * -- tap the left half to fly up-left, the right half to fly
 * up-right -- and gravity pulls her back down. Clouds are only
 * footing: landing on one (falling onto its top) bounces her back
 * up, and a grey cloud breaks after one bounce. The game is endless:
 * the camera follows her up, clouds recycle above, and she falls out
 * of the bottom when there is nothing left to bounce on.
 *
 * Score (user rule 2026-11-07): clouds give NO points; only coins
 * count, +1 each. There are no bombs and no timer, so the score only
 * ever goes up, and the settlement is muse_pet_round_coins() directly
 * (1 point per coin, 10 coins per round ceiling) -- no floor helper
 * is needed, because the score can never go negative. The 5-coin
 * entry fee was already charged at start.
 *
 * Physics is per 50 ms tick (MUSE_DOODLE_TICK_MS), tuned from the
 * 16Games-in-Cpp Doodle Jump reference (gravity 0.2, jump -10 per
 * frame at 60 fps): gravity 0.4 px/tick^2, flap -11 px/tick (about
 * 150 px of climb, two cloud gaps), bounce -9 px/tick (about 100 px,
 * one and a half gaps). Clouds sit ~70 px apart vertically, so a
 * flap always reaches the next row and a bounce usually does.
 * Horizontal speed is 3 px/tick and wraps around the field edges
 * (classic Doodle Jump); a tap sets both the flap and the direction.
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
#define MUSE_DOODLE_FLAP_VY (-11.0f)
#define MUSE_DOODLE_BOUNCE_VY (-9.0f)
#define MUSE_DOODLE_MAX_FALL_VY 16.0f
#define MUSE_DOODLE_H_SPEED 3.0f
/* Clouds sit this far apart vertically; the camera starts scrolling
 * once her centre rises above this line (field top + 70). */
#define MUSE_DOODLE_CLOUD_SPACING 70
#define MUSE_DOODLE_CAM_Y (MUSE_DOODLE_FIELD_Y + 70)
/* A moving cloud slides this fast, px/tick, bouncing off the field
 * edges. A new cloud gets a coin above it this often. */
#define MUSE_DOODLE_MOVE_SPEED 1.0f
#define MUSE_DOODLE_COIN_CHANCE_PCT 60u

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
    float vx; /* px/tick, set by the last tap, kept between taps */
    float vy; /* px/tick, positive is falling */
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
 * charges the entry fee again before calling this). */
void muse_doodle_restart(muse_doodle_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Applies gravity, moves her, bounces her off clouds, scrolls the
 * camera, recycles clouds above, collects coins, and ends the game
 * when she falls out of the bottom of the field. */
void muse_doodle_tick(muse_doodle_t *g, uint32_t ms);

/* A tap flaps her: vy is set to the flap speed and vx to the tap's
 * direction (dir < 0 flies left, dir > 0 flies right). No-op unless
 * PLAYING (a tap on an overlay never flaps). */
void muse_doodle_flap(muse_doodle_t *g, int dir);

/* Clouds currently on screen (active ones). */
int muse_doodle_cloud_count(const muse_doodle_t *g);

/* Coins currently on screen (active ones). */
int muse_doodle_coin_count(const muse_doodle_t *g);

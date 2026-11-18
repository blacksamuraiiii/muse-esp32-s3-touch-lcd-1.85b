/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_breakout.h -- 打砖块 logic (no LVGL, no ESP-IDF).
 *
 * A 240x240 field at (60,60), 6x4 = 24 bricks (+1 point each), a
 * 72x10 paddle on the bottom edge, one ball, ONE life: a missed ball
 * ends the round at once (user, 2026-11-18: no revive -- the earlier
 * three-lives rule let the first misses continue automatically). The
 * round ends when all 24 bricks are broken or the ball is missed; the
 * score is converted exactly once, at OVER, by muse_pet_round_coins()
 * (1 point per coin, 20 coins per round ceiling) -- never here.
 *
 * Ball speed is 120 px/s (MUSE_BREAKOUT_BALL_SPEED, user decision
 * 2026-11-18: the mockup's ~160 px/s was too fast for a 7-year-old).
 * The serve leaves (180,260) upward at 35..55 degrees to the
 * horizontal, vx sign random. vx can only change on a paddle hit
 * (offset * 90 px/s, clamped to +-220); speed never ramps up (v1 has
 * no difficulty curve -- if one is added later, the 220 cap and the
 * paddle's 260 px/s cap must be re-derived together, see below).
 *
 * Timing is in milliseconds and advances only through
 * muse_breakout_tick(), so the host test drives it deterministically
 * (no lv_tick_get() here). The UI calls tick every 50 ms and pauses
 * its timer while paused, so tick() also no-ops unless PLAYING.
 * Random serve angles use stdlib rand(), seeded by the caller (the UI
 * seeds once from lv_tick_get on the first start, as the snake page
 * does for food placement).
 *
 * Tilt numbers live here, not in the UI file, even though the
 * accelerometer read itself is UI-side: they are pure values (sign,
 * gain, dead zone, paddle speed cap), and the host harness checks
 * them through muse_breakout_tilt_target() / muse_breakout_glide()
 * below. The calibration is the 金币雨 one, verified on the real
 * board in 1.5.1: sign -1 (the chip's X axis points the other way
 * than the mockup assumed), gain 40 px per m/s^2 (a 35-degree tilt is
 * ~5.6 m/s^2, so 224 px of target travel -- more than the paddle's
 * 168 px run), dead zone 0.5 m/s^2 (~3 degrees, so a hand-held board
 * that is not perfectly level does not drift the paddle), low-pass
 * 0.35 (the UI keeps the filter state). The paddle glides toward the
 * tilt target instead of teleporting, at most
 * MUSE_BREAKOUT_PADDLE_MAX_PX_S -- 260 px/s, derived in the design
 * doc §4.2 from the worst case vx = 220 px/s: the ball crosses the
 * 240 px field in 240/220 = 1.09 s, the paddle's 168 px run at
 * 260 px/s takes 0.65 s, so it can still catch a ball headed for a
 * bottom corner; 金币雨's 130 px/s (1.29 s) could not. A finger drag
 * is 1:1 under the finger and is NOT capped -- the cap only applies
 * to the tilt glide.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_BREAKOUT_TICK_MS 50u
/* Field geometry, same 240x240 square the other games use. */
#define MUSE_BREAKOUT_FIELD_X 60
#define MUSE_BREAKOUT_FIELD_Y 60
#define MUSE_BREAKOUT_FIELD_SIZE 240
/* Ball: white dot, radius 5 px, constant speed (no ramp in v1). */
#define MUSE_BREAKOUT_BALL_R 5
#define MUSE_BREAKOUT_BALL_SPEED 120.0f
/* Paddle hits may push vx up to this; the paddle's own top speed is
 * derived from it (see MUSE_BREAKOUT_PADDLE_MAX_PX_S). */
#define MUSE_BREAKOUT_BALL_VX_MAX 220.0f
/* Serve: from the field centre, 35..55 degrees above horizontal. */
#define MUSE_BREAKOUT_SERVE_X 180.0f
#define MUSE_BREAKOUT_SERVE_Y 260.0f
#define MUSE_BREAKOUT_SERVE_ANGLE_MIN_DEG 35
#define MUSE_BREAKOUT_SERVE_ANGLE_MAX_DEG 55
/* Paddle: 72x10, top edge 14 px above the field bottom (y = 286).
 * Its centre runs 96..264 (half a paddle of margin on each side). */
#define MUSE_BREAKOUT_PADDLE_W 72
#define MUSE_BREAKOUT_PADDLE_H 10
#define MUSE_BREAKOUT_PADDLE_Y 286
#define MUSE_BREAKOUT_PADDLE_MIN_CX 96.0f
#define MUSE_BREAKOUT_PADDLE_MAX_CX 264.0f
/* Bricks: 6 columns x 4 rows, 36x16, 4 px column gap, 6 px row gap.
 * Column c starts at x = 62 + c*40, row r at y = 70 + r*22. */
#define MUSE_BREAKOUT_BRICK_COLS 6
#define MUSE_BREAKOUT_BRICK_ROWS 4
#define MUSE_BREAKOUT_BRICK_W 36
#define MUSE_BREAKOUT_BRICK_H 16
#define MUSE_BREAKOUT_BRICK_X0 62
#define MUSE_BREAKOUT_BRICK_Y0 70
#define MUSE_BREAKOUT_BRICK_PITCH_X 40
#define MUSE_BREAKOUT_BRICK_PITCH_Y 22
#define MUSE_BREAKOUT_BRICK_COUNT \
    (MUSE_BREAKOUT_BRICK_COLS * MUSE_BREAKOUT_BRICK_ROWS)
/* Lives per round: 1, i.e. a miss is game over (user, 2026-11-18).
 * The engine still counts them down, so raising this back to 3 restores
 * the revive behaviour without touching tick(). */
#define MUSE_BREAKOUT_LIVES 1u
/* Tilt calibration (金币雨's, 1.5.1 real-board verified): sign, gain
 * in px per m/s^2, dead zone in m/s^2, low-pass smoothing. */
#define MUSE_BREAKOUT_TILT_SIGN -1.0f
#define MUSE_BREAKOUT_TILT_GAIN_PX_PER_MS2 40.0f
#define MUSE_BREAKOUT_TILT_DEADZONE_MS2 0.5f
#define MUSE_BREAKOUT_TILT_SMOOTH 0.35f
/* Paddle top speed for the tilt glide, px/s (design §4.2: 260 beats
 * the worst-case ball crossing of 1.09 s with 0.65 s for the full
 * 168 px run; 130 px/s would need 1.29 s and lose the corner ball). */
#define MUSE_BREAKOUT_PADDLE_MAX_PX_S 260.0f
/* Paddle hit: vx changes by this much per unit of hit offset
 * (offset is (ball_x - paddle_cx) / 36, clamped to [-1, 1]). */
#define MUSE_BREAKOUT_PADDLE_KICK_PX_S 90.0f

typedef enum {
    MUSE_BREAKOUT_START = 0,
    MUSE_BREAKOUT_PLAYING,
    MUSE_BREAKOUT_PAUSED,
    MUSE_BREAKOUT_OVER,
} muse_breakout_phase_t;

/* What the last tick did, for the UI's sounds and floating "+1".
 * Only the first notable event of a tick is kept (a tick cannot
 * physically produce two: the brick rows end 134 px above the
 * paddle). event_x/event_y is the broken brick's centre when the
 * event is BRICK, and is left untouched otherwise. */
typedef enum {
    MUSE_BREAKOUT_EV_NONE = 0,
    MUSE_BREAKOUT_EV_BRICK,  /* a brick broke: +1, GOOD sound, float "+1" */
    MUSE_BREAKOUT_EV_PADDLE, /* the paddle caught the ball: PLACE sound */
    MUSE_BREAKOUT_EV_LOST,   /* the ball fell out: a life lost, BAD sound */
} muse_breakout_event_t;

typedef struct {
    muse_breakout_phase_t phase;
    float ball_x; /* ball centre, screen coordinates */
    float ball_y;
    float ball_vx; /* px/s */
    float ball_vy; /* px/s, negative is up */
    float paddle_cx; /* paddle centre x, screen coordinates */
    int32_t score; /* +1 per brick; never goes down, no penalty item */
    uint32_t lives; /* lives left, MUSE_BREAKOUT_LIVES at a fresh start */
    uint32_t bricks_left; /* bricks still standing */
    bool bricks[MUSE_BREAKOUT_BRICK_COUNT]; /* true = still standing */
    muse_breakout_event_t event; /* what the last tick did */
    float event_x; /* broken brick centre x (EV_BRICK only) */
    float event_y; /* broken brick centre y (EV_BRICK only) */
} muse_breakout_t;

void muse_breakout_init(muse_breakout_t *b);

/* START -> PLAYING, resetting the game. No-op from any other phase
 * (use restart for a new game after OVER/PAUSED). */
void muse_breakout_start(muse_breakout_t *b);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_breakout_pause(muse_breakout_t *b);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_breakout_resume(muse_breakout_t *b);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_breakout_restart(muse_breakout_t *b);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Moves the ball, bounces off the walls and the top, breaks at most
 * one brick per tick, catches on the paddle, and loses a life when
 * the ball falls out of the bottom. The round ends (phase OVER) when
 * the last brick breaks or the last life is lost. */
void muse_breakout_tick(muse_breakout_t *b, uint32_t ms);

/* Move the paddle: `cx` is the paddle centre x in screen coordinates,
 * clamped to 96..264. No-op unless PLAYING (the paddle never moves
 * on the overlays). A finger drag calls this directly (1:1); the
 * tilt glide calls muse_breakout_glide() first and passes the
 * glided position here. */
void muse_breakout_set_paddle(muse_breakout_t *b, float cx);

/* Top-left corner of a brick, in screen coordinates. */
void muse_breakout_brick_pos(int col, int row, float *x, float *y);

/* True while that brick is still standing. */
bool muse_breakout_brick_alive(const muse_breakout_t *b, int col, int row);

/* Tilt target for a low-passed accel x (m/s^2): the field centre
 * plus SIGN * GAIN * ax. Unclamped -- set_paddle() clamps it into
 * the paddle's run, the way 金币雨's update_tilt works. */
float muse_breakout_tilt_target(float ax_filt);

/* True when the low-passed accel x is outside the dead zone, i.e.
 * the tilt should move the paddle at all. Inside the dead zone the
 * paddle stays put -- holding the board level must not drag it back
 * to the centre. */
bool muse_breakout_tilt_active(float ax_filt);

/* Glide one step toward `target`: move `from` by at most
 * MUSE_BREAKOUT_PADDLE_MAX_PX_S * ms / 1000, so the paddle eases
 * instead of teleporting. The UI feeds this into set_paddle(). */
float muse_breakout_glide(float from, float target, uint32_t ms);

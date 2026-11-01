/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_whackamole.h -- whack-a-mole logic (no LVGL, no ESP-IDF).
 *
 * Six holes, 3 columns x 2 rows, on the 240x240 inscribed square
 * (mockup v9). A game lasts 30 seconds. One mole at a time (user
 * decision 2026-11-01: fewer moles, slower pace), staying up
 * MUSE_WHACKAMOLE_PACE_MS at first -- the spawn gap and the visible
 * time share that one curve -- and tightening every 10 seconds.
 * Tapping a hole while its mole is up scores one point and shows the
 * hit image for 200 ms, then the hole empties. A mole that is not
 * tapped in time runs away and counts as missed. The score is the hit
 * count (1 point per hit); there is no other scoring.
 *
 * Timing is in milliseconds and advances only through
 * muse_whackamole_tick(), so the host test drives it deterministically
 * (no lv_tick_get() here). The UI calls tick every 100 ms and pauses
 * its timer while paused, so tick() also no-ops unless PLAYING.
 * Random hole picks use stdlib rand(), seeded by the caller (the UI
 * seeds once from lv_tick_get on the first start, as the snake page
 * does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_WHACKAMOLE_HOLES 6
#define MUSE_WHACKAMOLE_COLS 3
#define MUSE_WHACKAMOLE_ROWS 2
#define MUSE_WHACKAMOLE_GAME_MS 30000u
/* One mole at a time (2026-11-01: was 2). The field is 80 px per hole on
 * a 360 px circle, so two simultaneous moles were more than a first-time
 * player could chase; one is also what keeps the pace readable. */
#define MUSE_WHACKAMOLE_MAX_UP 1
/* Pace: spawn gap and mole visible time, one curve (a mole never outlives
 * its gap). 1200 ms at the start, 100 ms tighter every 10 s of play,
 * floored at 900. These four are the only knobs for game speed; the host
 * harness derives its expectations from them. */
#define MUSE_WHACKAMOLE_PACE_MS 1200u
#define MUSE_WHACKAMOLE_PACE_STEP_MS 100u
#define MUSE_WHACKAMOLE_PACE_STEP_S 10u
#define MUSE_WHACKAMOLE_PACE_MIN_MS 900u
#define MUSE_WHACKAMOLE_HIT_SHOW_MS 200u
#define MUSE_WHACKAMOLE_TICK_MS 100u

typedef enum {
    MUSE_WHACKAMOLE_START = 0,
    MUSE_WHACKAMOLE_PLAYING,
    MUSE_WHACKAMOLE_PAUSED,
    MUSE_WHACKAMOLE_OVER,
} muse_whackamole_phase_t;

typedef enum {
    MUSE_WHACKAMOLE_EMPTY = 0,
    MUSE_WHACKAMOLE_UP,
    MUSE_WHACKAMOLE_HIT, /* hit image showing, 200 ms, then empty */
} muse_whackamole_hole_state_t;

typedef struct {
    muse_whackamole_hole_state_t state;
    uint32_t ms;         /* up-time while UP, show-time while HIT */
    uint32_t visible_ms; /* how long this mole stays up (UP only) */
} muse_whackamole_hole_t;

typedef struct {
    muse_whackamole_phase_t phase;
    muse_whackamole_hole_t holes[MUSE_WHACKAMOLE_HOLES];
    uint32_t score;   /* == hits: 1 point per hit */
    uint32_t hits;
    uint32_t missed;  /* moles that timed out untapped */
    uint32_t elapsed_ms;
    uint32_t spawn_ms; /* ms since the last spawn burst */
} muse_whackamole_t;

void muse_whackamole_init(muse_whackamole_t *g);

/* START -> PLAYING, resetting the game. Spawns one mole at once so
 * the field is never empty for the first spawn interval. No-op from
 * any other phase (use restart for a new game after OVER/PAUSED). */
void muse_whackamole_start(muse_whackamole_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_whackamole_pause(muse_whackamole_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_whackamole_resume(muse_whackamole_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_whackamole_restart(muse_whackamole_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Expires moles, shows hit images briefly, spawns, and ends the game
 * at 30 seconds (holes cleared, phase OVER). */
void muse_whackamole_tick(muse_whackamole_t *g, uint32_t ms);

/* Tap hole `hole` (0..5). True when a live mole was hit (it switches
 * to the hit image); false otherwise (empty hole, already-hit mole,
 * out-of-range hole, or not PLAYING) -- no penalty either way. */
bool muse_whackamole_tap(muse_whackamole_t *g, int hole);

/* Moles currently up (UP only; HIT-show moles do not block spawns). */
int muse_whackamole_up_count(const muse_whackamole_t *g);

/* Remaining whole seconds, rounded up (30 at start, 0 at the end). */
uint32_t muse_whackamole_remaining_s(const muse_whackamole_t *g);

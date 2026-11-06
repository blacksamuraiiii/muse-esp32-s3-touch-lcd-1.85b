/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_goldrain.h -- 金币雨 logic (no LVGL, no ESP-IDF).
 *
 * Coins (and the occasional bomb) fall from the top of the 240x240 field
 * at (60,60); the player moves Buttercup along the bottom to catch them.
 * A game lasts 30 seconds, no lives: catching a coin scores one point,
 * catching a bomb costs five points, and the game never ends early.
 * A coin or bomb that falls past the bottom is simply gone -- no penalty
 * for letting a bomb drop (only catching one hurts). The score can go
 * below zero mid-round (a bomb at 2 points leaves -3); at OVER the
 * settlement floors it at zero -- a round pays at most 10 coins and at
 * least 0 -- and the 5-coin entry fee was already charged at start.
 *
 * Fall speed is deliberately slow (user request 2026-11-06, "金币下降的
 * 速度不要太快"): 90 px/s at the start, +4 px/s per second of play,
 * capped at 150 px/s. The spawn gap starts at 950 ms and tightens 10 ms
 * per second to a 600 ms floor; at most 6 items fall at once. The score
 * is +1 per coin caught and -5 per bomb caught, so it can go negative;
 * the coin conversion lives in muse_pet_round_coins() (1 point per coin,
 * 10 coins per round ceiling), never here, and the UI floors the score
 * at 0 before calling it (muse_goldrain_settle_score(), below), so a
 * round that ends below zero pays 0 coins, never a negative amount.
 *
 * Timing is in milliseconds and advances only through
 * muse_goldrain_tick(), so the host test drives it deterministically
 * (no lv_tick_get() here). The UI calls tick every 50 ms and pauses
 * its timer while paused, so tick() also no-ops unless PLAYING.
 * Random spawn picks use stdlib rand(), seeded by the caller (the UI
 * seeds once from lv_tick_get on the first start, as the snake page
 * does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_GOLDRAIN_GAME_MS 30000u
#define MUSE_GOLDRAIN_TICK_MS 50u
/* Points a caught bomb costs (user rule 2026-11-06). */
#define MUSE_GOLDRAIN_BOMB_PENALTY 5
#define MUSE_GOLDRAIN_MAX_ITEMS 8
#define MUSE_GOLDRAIN_MAX_FALLING 6
/* Field geometry, same 240x240 square the other games use. */
#define MUSE_GOLDRAIN_FIELD_X 60
#define MUSE_GOLDRAIN_FIELD_Y 60
#define MUSE_GOLDRAIN_FIELD_SIZE 240
/* Art sizes (muse_goldrain_art.h): the coin is 18 px, Buttercup 53x66. */
#define MUSE_GOLDRAIN_COIN_PX 18
#define MUSE_GOLDRAIN_SPRITE_W 53
#define MUSE_GOLDRAIN_SPRITE_H 66
/* The catch line is the sprite's top edge: the sprite stands on the
 * field's bottom edge, so a coin is caught when its bottom edge reaches
 * it. */
#define MUSE_GOLDRAIN_CATCH_Y \
    (MUSE_GOLDRAIN_FIELD_Y + MUSE_GOLDRAIN_FIELD_SIZE - MUSE_GOLDRAIN_SPRITE_H)
/* Fall speed curve (user: not too fast): 90 px/s at the start, +4 px/s
 * per second, capped at 150 px/s. Spawn gap: 950 ms at the start, -10 ms
 * per second, floored at 600 ms. Bomb chance per spawn: 15%. These are
 * the only knobs for game speed; the host harness derives its
 * expectations from them. */
#define MUSE_GOLDRAIN_SPEED_START_PX_S 90.0f
#define MUSE_GOLDRAIN_SPEED_STEP_PX_S 4.0f
#define MUSE_GOLDRAIN_SPEED_MAX_PX_S 150.0f
#define MUSE_GOLDRAIN_SPAWN_START_MS 950u
#define MUSE_GOLDRAIN_SPAWN_STEP_MS 10u
#define MUSE_GOLDRAIN_SPAWN_MIN_MS 600u
#define MUSE_GOLDRAIN_BOMB_CHANCE_PCT 15u

typedef enum {
    MUSE_GOLDRAIN_START = 0,
    MUSE_GOLDRAIN_PLAYING,
    MUSE_GOLDRAIN_PAUSED,
    MUSE_GOLDRAIN_OVER,
} muse_goldrain_phase_t;

typedef enum {
    MUSE_GOLDRAIN_COIN = 0,
    MUSE_GOLDRAIN_BOMB,
} muse_goldrain_kind_t;

typedef struct {
    bool active;
    muse_goldrain_kind_t kind;
    float x; /* left edge, screen coordinates */
    float y; /* top edge, screen coordinates */
} muse_goldrain_item_t;

typedef struct {
    muse_goldrain_phase_t phase;
    muse_goldrain_item_t items[MUSE_GOLDRAIN_MAX_ITEMS];
    float catcher_cx; /* sprite centre x, screen coordinates */
    int32_t score;    /* +1 per coin, -5 per bomb; may go negative */
    uint32_t caught;
    uint32_t missed;  /* coins that fell past the bottom */
    uint32_t elapsed_ms;
    uint32_t spawn_ms; /* ms since the last spawn */
} muse_goldrain_t;

void muse_goldrain_init(muse_goldrain_t *g);

/* START -> PLAYING, resetting the game. Spawns one coin at once so
 * the field is never empty for the first spawn interval. No-op from
 * any other phase (use restart for a new game after OVER/PAUSED). */
void muse_goldrain_start(muse_goldrain_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_goldrain_pause(muse_goldrain_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_goldrain_resume(muse_goldrain_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_goldrain_restart(muse_goldrain_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Spawns, falls the items, catches and misses, and ends the game at
 * 30 seconds (items cleared, phase OVER). Bombs never end the game. */
void muse_goldrain_tick(muse_goldrain_t *g, uint32_t ms);

/* Move the catcher: `cx` is the sprite centre x in screen coordinates,
 * clamped to the field (half a sprite of margin on each side). No-op
 * unless PLAYING (the catcher never moves on the overlays). */
void muse_goldrain_set_catcher(muse_goldrain_t *g, float cx);

/* Fall speed right now, px/s (the curve above, for the UI and the
 * harness). */
float muse_goldrain_speed_px_s(const muse_goldrain_t *g);

/* Spawn gap right now, ms (the curve above). */
uint32_t muse_goldrain_spawn_gap_ms(const muse_goldrain_t *g);

/* Items currently falling. */
int muse_goldrain_falling_count(const muse_goldrain_t *g);

/* Remaining whole seconds, rounded up (30 at start, 0 at the end). */
uint32_t muse_goldrain_remaining_s(const muse_goldrain_t *g);

/* Final score -> the value handed to muse_pet_round_coins(): a round
 * that ends below zero pays 0 coins (user rule 2026-11-06: at most 10
 * coins, at least 0). The 10-coin ceiling stays in round_coins(); this
 * only floors at zero, so it never subtracts from the wallet. */
static inline uint32_t muse_goldrain_settle_score(int32_t score)
{
    return score < 0 ? 0u : (uint32_t)score;
}

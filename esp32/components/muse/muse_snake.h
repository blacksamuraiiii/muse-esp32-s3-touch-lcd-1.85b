/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_snake.h -- Snake game logic (no LVGL, no ESP-IDF).
 *
 * Pure logic on purpose: the UI (muse_snake_ui.c) owns all LVGL objects,
 * this file owns only the board. It compiles on the host with plain cc,
 * so tests/test_snake_logic.py can verify every rule without a board.
 *
 * Board: 12 cols x 12 rows, cells (x, y) with x right, y down, both
 * 0..11. The snake is stored head-first in body[0..len-1].
 *
 * Rules (user decision 2026-10-31, mockup snake-mockup-v1):
 *   - Initial length 3, moving right from the centre.
 *   - One tick moves the head one cell in the current direction.
 *   - Steering: a 180-degree reversal is ignored, and each tick takes
 *     at most one turn -- rapid taps resolve to the last VALID
 *     direction (valid = not the opposite of the current direction).
 *   - Eating food scores 10 and grows the snake by one cell.
 *   - Hitting a wall or the snake's own body ends the game (OVER).
 *   - No coins, no NVS here: the entry fee lives in the UI layer
 *     (PET_FEE through the pet worker), this file only plays.
 *   - Progress lives in RAM (leaving the page pauses, re-entering
 *     resumes; a reboot restarts from START).
 *
 * v1 defaults (change here to retune): tick 200 ms (the UI timer rate),
 * +10 per food, initial length 3, 12x12 board.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_SNAKE_COLS 12
#define MUSE_SNAKE_ROWS 12
#define MUSE_SNAKE_CELLS (MUSE_SNAKE_COLS * MUSE_SNAKE_ROWS)
#define MUSE_SNAKE_INIT_LEN 3
#define MUSE_SNAKE_FOOD_SCORE 10
/* Tick interval, milliseconds. v1 fixed default: the UI timer runs at
 * this rate; change it here to retune game speed. */
#define MUSE_SNAKE_TICK_MS 200

typedef enum {
    MUSE_SNAKE_UP = 0,
    MUSE_SNAKE_DOWN,
    MUSE_SNAKE_LEFT,
    MUSE_SNAKE_RIGHT,
} muse_snake_dir_t;

typedef enum {
    MUSE_SNAKE_START = 0, /* fresh board, not started yet */
    MUSE_SNAKE_PLAYING,
    MUSE_SNAKE_PAUSED,
    MUSE_SNAKE_OVER,      /* wall or self collision */
} muse_snake_state_t;

typedef struct {
    uint8_t x, y;
} muse_snake_cell_t;

typedef struct {
    muse_snake_state_t state;
    muse_snake_dir_t dir;               /* direction of the last move */
    uint16_t score;                     /* foods eaten x 10 */
    uint8_t len;                        /* 3..144, body[0] is the head */
    muse_snake_cell_t body[MUSE_SNAKE_CELLS];
    muse_snake_cell_t food;
    muse_snake_dir_t pending;           /* queued turn, applied next tick */
    bool has_pending;
} muse_snake_t;

/* Fresh game in START state: length 3 moving right from the centre,
 * score 0, food placed on a random free cell. The caller seeds the C
 * PRNG (srand) -- the UI seeds it from lv_tick_get() at first start,
 * tests seed it for determinism. */
void muse_snake_init(muse_snake_t *g);

/* START -> PLAYING. No-op in any other state (restart covers OVER). */
void muse_snake_start(muse_snake_t *g);

/* PLAYING -> PAUSED. No-op in any other state. */
void muse_snake_pause(muse_snake_t *g);

/* PAUSED -> PLAYING. No-op in any other state. */
void muse_snake_resume(muse_snake_t *g);

/* Any state -> a fresh PLAYING game (initial snake, score 0, new food). */
void muse_snake_restart(muse_snake_t *g);

/* Queue a turn, applied at the next tick. Only in PLAYING; a 180-degree
 * reversal of the current direction is ignored, and each tick takes at
 * most one turn, so rapid taps resolve to the last valid direction. */
void muse_snake_set_dir(muse_snake_t *g, muse_snake_dir_t dir);

/* Advance one tick when PLAYING (no-op otherwise): apply the queued
 * turn, move the head, grow + score on food, end on wall/self hit.
 * The tail cell vacates unless the snake eats, so moving into the
 * cell the tail just left is legal. */
void muse_snake_tick(muse_snake_t *g);

/* Place food at (x, y). False (no change) when out of bounds or on the
 * snake. Mainly for tests: the UI never calls it, ticks place food
 * randomly on a free cell. */
bool muse_snake_set_food(muse_snake_t *g, uint8_t x, uint8_t y);

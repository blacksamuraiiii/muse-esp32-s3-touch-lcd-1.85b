/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_mathpractice.h -- 加减练习 logic (no LVGL, no ESP-IDF).
 *
 * A 30-second round of 20以内加减法 judgement: the screen shows one
 * equation ("7 + 8 = 15") whose displayed answer is right about half
 * the time and wrong about half the time (user rule 2026-11-08, 1:1),
 * and the player taps the green check (right) or the red cross
 * (wrong). A correct judgement scores one point; a wrong judgement
 * scores nothing and never subtracts. The next equation appears at
 * once, so the round is as many questions as fit in 30 seconds.
 *
 * Equations are always within 20 (user rule): addition a + b with
 * a + b <= 20, subtraction a - b with a >= b so the answer is >= 0.
 * A wrong displayed answer is a plausible near-miss of the true one
 * (off by 1, 2 or 10, staying inside 0..20), never an absurd value
 * like 100 -- a first-grader should have to compute, not spot a joke.
 * The 1:1 correct/wrong split is per-question coin flip (rand() % 2),
 * so over a round it averages 1:1.
 *
 * Score only ever goes up, so the settlement is
 * muse_pet_round_coins() directly (1 point per coin, 10 coins per
 * round ceiling). The 5-coin entry fee was already charged at start.
 *
 * Timing is in milliseconds and advances only through
 * muse_mathpractice_tick(), so the host test drives it
 * deterministically (no lv_tick_get() here). The UI calls tick every
 * 50 ms and pauses its timer while paused, so tick() also no-ops
 * unless PLAYING. Random picks use stdlib rand(), seeded by the
 * caller (the UI seeds once from lv_tick_get on the first start, as
 * the snake page does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_MATHPRACTICE_GAME_MS 30000u
#define MUSE_MATHPRACTICE_TICK_MS 50u
/* Operands and answers all live in 0..20 (user rule 2026-11-08). */
#define MUSE_MATHPRACTICE_MAX_NUM 20

typedef enum {
    MUSE_MATHPRACTICE_START = 0,
    MUSE_MATHPRACTICE_PLAYING,
    MUSE_MATHPRACTICE_PAUSED,
    MUSE_MATHPRACTICE_OVER,
} muse_mathpractice_phase_t;

typedef struct {
    muse_mathpractice_phase_t phase;
    int a;           /* first operand, 0..20 */
    int b;           /* second operand, 0..20 */
    bool is_add;     /* true: a + b, false: a - b */
    int shown;       /* the answer displayed, 0..20 */
    bool is_correct; /* whether shown == the true answer */
    uint32_t score;    /* +1 per correct judgement, never decreases */
    uint32_t answered; /* questions judged so far */
    uint32_t elapsed_ms;
} muse_mathpractice_t;

void muse_mathpractice_init(muse_mathpractice_t *g);

/* START -> PLAYING, resetting the game and dealing the first
 * equation. No-op from any other phase (use restart for a new game
 * after OVER/PAUSED). */
void muse_mathpractice_start(muse_mathpractice_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_mathpractice_pause(muse_mathpractice_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_mathpractice_resume(muse_mathpractice_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_mathpractice_restart(muse_mathpractice_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Ends the game at 30 seconds (phase OVER). */
void muse_mathpractice_tick(muse_mathpractice_t *g, uint32_t ms);

/* Judge the current equation: `judged_correct` is true when the
 * player tapped the green check (thinks it is right), false for the
 * red cross. Returns true when the judgement was right (score +1),
 * false otherwise (score unchanged). Either way the next equation is
 * dealt at once. No-op (returns false) unless PLAYING. */
bool muse_mathpractice_answer(muse_mathpractice_t *g, bool judged_correct);

/* The true answer of the current equation (a + b or a - b). */
int muse_mathpractice_true_answer(const muse_mathpractice_t *g);

/* Remaining whole seconds, rounded up (30 at start, 0 at the end). */
uint32_t muse_mathpractice_remaining_s(const muse_mathpractice_t *g);

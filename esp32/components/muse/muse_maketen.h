/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_maketen.h -- 凑十练习 logic (no LVGL, no ESP-IDF).
 *
 * A 30-second round of 凑十法 judgement, same shape as 加减练习
 * (user rule 2026-11-08: "按照我们之前加减练习的分支就好了"):
 * the screen shows one addition ("8 + 5 = ?") and one proposed
 * make-ten solution ("8 + 2 + 3 = 13"), and the player taps the
 * green check (the solution is right) or the red cross (it is
 * wrong). A correct judgement scores one point; a wrong judgement
 * scores nothing and never subtracts. The next question appears at
 * once, so the round is as many questions as fit in 30 seconds.
 *
 * A question is a + b with a in 6..9 (the number to complete to 10),
 * b in 3..9, a + b in 11..18, and b larger than the complement of a
 * (so the split leaves a remainder of at least 1). The complement
 * is c = 10 - a and the remainder is r = b - c; the correct solution
 * is a + c + r = 10 + r. A wrong solution is one of two kinds,
 * picked at random: a wrong split (c' is not the complement of a,
 * so the whole chain is false even though c' + r' still equals b)
 * or a wrong final sum (the split is right, the displayed
 * 10 + r' is off by one). Either way the displayed equation is
 * false, and the player must check the split, not just the answer
 * -- a wrong split can still land on the right final number.
 * The 1:1 correct/wrong split is per-question coin flip (rand() % 2),
 * so over a round it averages 1:1.
 *
 * Score only ever goes up, so the settlement is
 * muse_pet_round_coins() directly (1 point per coin, 10 coins per
 * round ceiling). The 5-coin entry fee was already charged at start.
 *
 * Timing is in milliseconds and advances only through
 * muse_maketen_tick(), so the host test drives it
 * deterministically (no lv_tick_get() here). The UI calls tick every
 * 50 ms and pauses its timer while paused, so tick() also no-ops
 * unless PLAYING. Random picks use stdlib rand(), seeded by the
 * caller (the UI seeds once from lv_tick_get on the first start, as
 * the snake page does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_MAKETEN_GAME_MS 30000u
#define MUSE_MAKETEN_TICK_MS 50u
/* The number to complete to 10 lives in 6..9 (complement 1..4);
 * the number to split lives in 3..9 (b >= 3 so a wrong split always
 * has a candidate: with b == 2 and a == 9 the only split is 1 + 1
 * and no wrong first part exists); sums live in 11..18. */
#define MUSE_MAKETEN_A_MIN 6
#define MUSE_MAKETEN_A_MAX 9
#define MUSE_MAKETEN_B_MIN 3
#define MUSE_MAKETEN_B_MAX 9
#define MUSE_MAKETEN_SUM_MAX 18

typedef enum {
    MUSE_MAKETEN_START = 0,
    MUSE_MAKETEN_PLAYING,
    MUSE_MAKETEN_PAUSED,
    MUSE_MAKETEN_OVER,
} muse_maketen_phase_t;

typedef struct {
    muse_maketen_phase_t phase;
    int a;           /* the number to complete to 10, 6..9 */
    int b;           /* the number to split, 2..9 */
    int c;           /* complement of a: 10 - a, 1..4 */
    int r;           /* remainder: b - c, 1..8 */
    int shown_c;     /* the split's first part as displayed */
    int shown_r;     /* the split's second part as displayed */
    int shown_sum;   /* the final sum as displayed (10 + r, or wrong) */
    bool is_correct; /* whether the displayed solution is right */
    uint32_t score;    /* +1 per correct judgement, never decreases */
    uint32_t answered; /* questions judged so far */
    uint32_t elapsed_ms;
} muse_maketen_t;

void muse_maketen_init(muse_maketen_t *g);

/* START -> PLAYING, resetting the game and dealing the first
 * question. No-op from any other phase (use restart for a new game
 * after OVER/PAUSED). */
void muse_maketen_start(muse_maketen_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_maketen_pause(muse_maketen_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_maketen_resume(muse_maketen_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_maketen_restart(muse_maketen_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING.
 * Ends the game at 30 seconds (phase OVER). */
void muse_maketen_tick(muse_maketen_t *g, uint32_t ms);

/* Judge the current solution: `judged_correct` is true when the
 * player tapped the green check (thinks it is right), false for the
 * red cross. Returns true when the judgement was right (score +1),
 * false otherwise (score unchanged). Either way the next question is
 * dealt at once. No-op (returns false) unless PLAYING. */
bool muse_maketen_answer(muse_maketen_t *g, bool judged_correct);

/* The true final sum of the current question (a + b). */
int muse_maketen_true_answer(const muse_maketen_t *g);

/* Remaining whole seconds, rounded up (30 at start, 0 at the end). */
uint32_t muse_maketen_remaining_s(const muse_maketen_t *g);

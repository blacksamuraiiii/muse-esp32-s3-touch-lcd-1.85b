/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_maketen.h -- 凑十法练习 logic (no LVGL, no ESP-IDF).
 *
 * A 30-second round of comparison with a make-ten hint, the
 * 2026-11-08 rework of 凑十练习 (the true/false version was
 * cancelled the same day; the user then asked for this compare
 * version, "游戏名称改成凑十法练习", on the same branch): the
 * screen shows one addition with a ? where the comparison goes
 * ("5 + 8 ? 13"), a small hint line with the two make-ten steps
 * ("10-8=2  5-2=3"), and three buttons below -- >, < and =. The
 * player taps the comparison that makes the statement true. A
 * correct pick scores one point; a wrong pick scores nothing and
 * never subtracts. The next question appears at once, so the round
 * is as many questions as fit in 30 seconds.
 *
 * One question practises three things at once (user idea
 * 2026-11-08): the hint gives the make-ten steps, the player still
 * has to compute the sum (10 + 3 = 13) and then compare it with
 * the number on the right. The hint is a scaffold, not the answer:
 * it shows the thinking -- how much the big addend needs to reach
 * 10, and what is left of the small addend after giving it -- never
 * the sum and never the comparison. The steps are subtractions
 * (user correction 2026-11-08, "是不是这样好理解一些"): 10 - big
 * is what the big addend is short of 10, small - that is what
 * remains; the addition split ("2+3=5", "2+8=10") only states
 * results and hides where the 2 comes from.
 *
 * A question is a + b with a, b in 2..9 and a + b in 11..18 (so the
 * make-ten split always applies). The comparison number d is the
 * true sum about a third of the time (answer =), a near-miss above
 * it about a third (answer <) and a near-miss below it about a
 * third (answer >) -- the near-misses are off by 1 or 2, so the
 * player has to compute, not guess. The hint follows the口诀
 * "看大数，分小数，凑成十，加剩数": big is the larger addend
 * (6..9), small the smaller (2..9); need = 10 - big (1..4) is
 * what big is short of 10, left = small - need (>= 1, since
 * a + b > 10) is what remains of small after giving need to big.
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
/* Addends live in 2..9; sums live in 11..18 (make-ten range);
 * the comparison number lives in 0..20. */
#define MUSE_MAKETEN_A_MIN 2
#define MUSE_MAKETEN_A_MAX 9
#define MUSE_MAKETEN_B_MIN 2
#define MUSE_MAKETEN_B_MAX 9
#define MUSE_MAKETEN_SUM_MIN 11
#define MUSE_MAKETEN_SUM_MAX 18
#define MUSE_MAKETEN_MAX_NUM 20

typedef enum {
    MUSE_MAKETEN_CMP_GT = 0, /* a + b > d */
    MUSE_MAKETEN_CMP_LT,     /* a + b < d */
    MUSE_MAKETEN_CMP_EQ,     /* a + b == d */
} muse_maketen_cmp_t;

typedef enum {
    MUSE_MAKETEN_START = 0,
    MUSE_MAKETEN_PLAYING,
    MUSE_MAKETEN_PAUSED,
    MUSE_MAKETEN_OVER,
} muse_maketen_phase_t;

typedef struct {
    muse_maketen_phase_t phase;
    int a;           /* first addend, 2..9 */
    int b;           /* second addend, 2..9 */
    int d;           /* the number to compare against, 0..20 */
    muse_maketen_cmp_t correct; /* the comparison that is true */
    int hint_big;    /* the larger addend, 6..9 */
    int hint_small;  /* the smaller addend, 2..9 */
    int hint_need;   /* 10 - hint_big: what big is short of 10, 1..4 */
    int hint_left;   /* hint_small - hint_need: what remains, >= 1 */
    uint32_t score;    /* +1 per correct pick, never decreases */
    uint32_t answered; /* questions answered so far */
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

/* Pick a comparison: `cmp` is the button the player tapped
 * (GT for >, LT for <, EQ for =). Returns true when the pick was
 * right (score +1), false otherwise (score unchanged). Either way
 * the next question is dealt at once. No-op (returns false) unless
 * PLAYING. */
bool muse_maketen_answer(muse_maketen_t *g, muse_maketen_cmp_t cmp);

/* The true sum of the current question (a + b). */
int muse_maketen_true_answer(const muse_maketen_t *g);

/* Remaining whole seconds, rounded up (30 at start, 0 at the end). */
uint32_t muse_maketen_remaining_s(const muse_maketen_t *g);

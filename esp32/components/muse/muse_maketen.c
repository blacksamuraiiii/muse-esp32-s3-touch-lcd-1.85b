/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_maketen.c -- 凑十法练习 logic, see muse_maketen.h.
 *
 * Each question is one comparison, dealt by next_question():
 * a, b in 2..9, a + b in 11..18. The comparison number d is the
 * true sum (answer =), a near-miss above it (answer <) or a
 * near-miss below it (answer >), each about a third of the time;
 * the near-misses are off by 1 or 2 and stay inside 0..20. The
 * hint is the two make-ten steps as subtractions (user correction
 * 2026-11-08): need = 10 - big is what the big addend is short of
 * 10, left = small - need is what remains of the small addend
 * after giving need to big. big is 6..9 (a + b >= 11 forces the
 * larger addend to at least 6), so need is 1..4; left is >= 1
 * exactly when a + b > 10, which the dealer guarantees.
 *
 * A pick only ever adds: right is +1, wrong is +0. The round
 * ends only on the 30 s timer -- there is no early end and no lives.
 */
#include "muse_maketen.h"

#include <stdlib.h>

static void next_question(muse_maketen_t *g)
{
    /* Deal the addition: a, b in 2..9, a + b in 11..18. The loop
     * retries until the constraints hold; a and b are independent
     * draws, so it terminates quickly. */
    int sum;
    for (;;) {
        g->a = MUSE_MAKETEN_A_MIN +
               rand() % (MUSE_MAKETEN_A_MAX - MUSE_MAKETEN_A_MIN + 1);
        g->b = MUSE_MAKETEN_B_MIN +
               rand() % (MUSE_MAKETEN_B_MAX - MUSE_MAKETEN_B_MIN + 1);
        sum = g->a + g->b;
        if (sum >= MUSE_MAKETEN_SUM_MIN && sum <= MUSE_MAKETEN_SUM_MAX) {
            break;
        }
    }
    /* The comparison number: the true sum (answer =), a near-miss
     * above it (answer <) or a near-miss below it (answer >), each
     * about a third of the time. The near-miss is off by 1 or 2 and
     * stays inside 0..20: sums are 11..18, so both directions always
     * have room. */
    int kind = rand() % 3;
    int delta = 1 + rand() % 2;
    if (kind == 0) {
        g->d = sum;
        g->correct = MUSE_MAKETEN_CMP_EQ;
    } else if (kind == 1) {
        g->d = sum + delta;
        g->correct = MUSE_MAKETEN_CMP_LT;
    } else {
        g->d = sum - delta;
        g->correct = MUSE_MAKETEN_CMP_GT;
    }
    /* The hint: the two make-ten steps, as subtractions. big is the
     * larger addend, small the smaller; need = 10 - big is what big
     * is short of 10, left = small - need is what remains of small
     * after giving need to big. The UI prints "10-big=need
     * small-need=left" (e.g. 5 + 8 -> "10-8=2  5-2=3"). */
    g->hint_big = g->a > g->b ? g->a : g->b;
    g->hint_small = g->a > g->b ? g->b : g->a;
    g->hint_need = 10 - g->hint_big;
    g->hint_left = g->hint_small - g->hint_need;
}

static void reset_game(muse_maketen_t *g)
{
    g->score = 0;
    g->answered = 0;
    g->elapsed_ms = 0;
    next_question(g);
}

void muse_maketen_init(muse_maketen_t *g)
{
    g->phase = MUSE_MAKETEN_START;
    reset_game(g);
}

void muse_maketen_start(muse_maketen_t *g)
{
    if (g->phase != MUSE_MAKETEN_START) {
        return;
    }
    reset_game(g);
    g->phase = MUSE_MAKETEN_PLAYING;
}

void muse_maketen_pause(muse_maketen_t *g)
{
    if (g->phase == MUSE_MAKETEN_PLAYING) {
        g->phase = MUSE_MAKETEN_PAUSED;
    }
}

void muse_maketen_resume(muse_maketen_t *g)
{
    if (g->phase == MUSE_MAKETEN_PAUSED) {
        g->phase = MUSE_MAKETEN_PLAYING;
    }
}

void muse_maketen_restart(muse_maketen_t *g)
{
    reset_game(g);
    g->phase = MUSE_MAKETEN_PLAYING;
}

void muse_maketen_tick(muse_maketen_t *g, uint32_t ms)
{
    if (g->phase != MUSE_MAKETEN_PLAYING) {
        return;
    }
    g->elapsed_ms += ms;
    if (g->elapsed_ms >= MUSE_MAKETEN_GAME_MS) {
        g->phase = MUSE_MAKETEN_OVER;
    }
}

bool muse_maketen_answer(muse_maketen_t *g, muse_maketen_cmp_t cmp)
{
    if (g->phase != MUSE_MAKETEN_PLAYING) {
        return false;
    }
    bool right = (cmp == g->correct);
    if (right) {
        g->score++;
    }
    g->answered++;
    next_question(g);
    return right;
}

int muse_maketen_true_answer(const muse_maketen_t *g)
{
    return g->a + g->b;
}

uint32_t muse_maketen_remaining_s(const muse_maketen_t *g)
{
    if (g->elapsed_ms >= MUSE_MAKETEN_GAME_MS) {
        return 0;
    }
    return (MUSE_MAKETEN_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

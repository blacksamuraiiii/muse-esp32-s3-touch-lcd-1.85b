/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_mathpractice.c -- 数学练习 logic, see muse_mathpractice.h.
 *
 * deal() flips a coin for the kind and deals one legal question:
 *
 *   ADD_TEN: a in 2..9, b in max(2, 11 - a)..min(9, 18 - a), so
 *            a + b lands in 11..18 by construction (no retry loop)
 *            and both addends are single digits.
 *   SUB_TEN: a in 11..18, b in (a % 10 + 1)..9, so the ones digit is
 *            always smaller than b -- the subtraction borrows ten --
 *            and the difference lands in 2..9 by construction.
 *
 * The comparison number is the true value or a near-miss: 0, +1, -1,
 * +2, -2 picked uniformly, so a round is 20% equal and 40% each way.
 * The near-misses never collide with the true value, and the value is
 * at least 2, so no negative comparison number can appear.
 *
 * A pick only ever adds: right is +1, wrong is +0. The verdict then
 * holds for FEEDBACK_MS; the next question is dealt when it expires,
 * never at the tap -- that is what makes "点击之后提示正确还是错误"
 * possible and what blocks a double tap from skipping a question.
 */
#include "muse_mathpractice.h"

#include <stdlib.h>

static int eq_value(const muse_mathpractice_t *g)
{
    return g->kind == MUSE_MATHPRACTICE_KIND_ADD_TEN ? g->a + g->b : g->a - g->b;
}

static void deal(muse_mathpractice_t *g)
{
    g->kind = (rand() % 2 == 0) ? MUSE_MATHPRACTICE_KIND_ADD_TEN
                                : MUSE_MATHPRACTICE_KIND_SUB_TEN;
    if (g->kind == MUSE_MATHPRACTICE_KIND_ADD_TEN) {
        g->a = MUSE_MATHPRACTICE_ADDEND_MIN +
               rand() % (MUSE_MATHPRACTICE_ADDEND_MAX - MUSE_MATHPRACTICE_ADDEND_MIN + 1);
        int lo = MUSE_MATHPRACTICE_SUM_MIN - g->a;
        int hi = MUSE_MATHPRACTICE_SUM_MAX - g->a;
        if (lo < MUSE_MATHPRACTICE_ADDEND_MIN) {
            lo = MUSE_MATHPRACTICE_ADDEND_MIN;
        }
        if (hi > MUSE_MATHPRACTICE_ADDEND_MAX) {
            hi = MUSE_MATHPRACTICE_ADDEND_MAX;
        }
        g->b = lo + rand() % (hi - lo + 1);
    } else {
        g->a = MUSE_MATHPRACTICE_MINUEND_MIN +
               rand() % (MUSE_MATHPRACTICE_MINUEND_MAX - MUSE_MATHPRACTICE_MINUEND_MIN + 1);
        /* Borrow ten: the subtrahend must exceed the ones digit. */
        int lo = (g->a % 10) + 1;
        g->b = lo + rand() % (MUSE_MATHPRACTICE_ADDEND_MAX - lo + 1);
    }
    int value = eq_value(g);
    /* The comparison number: true value, or a near-miss +-1 / +-2.
     * value is >= 2 in both kinds (ADD_TEN sums to 11..18, SUB_TEN
     * differences are 2..9), and the smallest delta is -2, so d >= 0
     * always -- no clamp needed (audit 2026-11-08 removed a dead one). */
    static const int deltas[] = {0, 1, -1, 2, -2};
    g->d = value + deltas[rand() % (int)(sizeof(deltas) / sizeof(deltas[0]))];
    g->correct = value > g->d ? MUSE_MATHPRACTICE_CMP_GT
                : value < g->d ? MUSE_MATHPRACTICE_CMP_LT
                               : MUSE_MATHPRACTICE_CMP_EQ;
    g->feedback_ms = 0;
}

static void reset_game(muse_mathpractice_t *g)
{
    g->score = 0;
    g->answered = 0;
    g->elapsed_ms = 0;
    g->last_right = false;
    deal(g);
}

void muse_mathpractice_init(muse_mathpractice_t *g)
{
    g->phase = MUSE_MATHPRACTICE_START;
    reset_game(g);
}

void muse_mathpractice_start(muse_mathpractice_t *g)
{
    if (g->phase != MUSE_MATHPRACTICE_START) {
        return;
    }
    reset_game(g);
    g->phase = MUSE_MATHPRACTICE_PLAYING;
}

void muse_mathpractice_pause(muse_mathpractice_t *g)
{
    if (g->phase == MUSE_MATHPRACTICE_PLAYING) {
        g->phase = MUSE_MATHPRACTICE_PAUSED;
    }
}

void muse_mathpractice_resume(muse_mathpractice_t *g)
{
    if (g->phase == MUSE_MATHPRACTICE_PAUSED) {
        g->phase = MUSE_MATHPRACTICE_PLAYING;
    }
}

void muse_mathpractice_restart(muse_mathpractice_t *g)
{
    reset_game(g);
    g->phase = MUSE_MATHPRACTICE_PLAYING;
}

void muse_mathpractice_tick(muse_mathpractice_t *g, uint32_t ms)
{
    if (g->phase != MUSE_MATHPRACTICE_PLAYING) {
        return;
    }
    g->elapsed_ms += ms;
    if (g->elapsed_ms >= MUSE_MATHPRACTICE_GAME_MS) {
        g->phase = MUSE_MATHPRACTICE_OVER;
        g->feedback_ms = 0;
        return;
    }
    /* The verdict window: nothing is dealt until it runs out, so the
     * player always sees the grade. */
    if (g->feedback_ms != 0) {
        if (ms >= g->feedback_ms) {
            deal(g);
        } else {
            g->feedback_ms -= ms;
        }
    }
}

bool muse_mathpractice_answer(muse_mathpractice_t *g, muse_mathpractice_cmp_t cmp)
{
    if (g->phase != MUSE_MATHPRACTICE_PLAYING || g->feedback_ms != 0) {
        return false;
    }
    bool right = (cmp == g->correct);
    if (right) {
        g->score++;
    }
    g->answered++;
    g->last_right = right;
    g->feedback_ms = MUSE_MATHPRACTICE_FEEDBACK_MS;
    return right;
}

int muse_mathpractice_value(const muse_mathpractice_t *g)
{
    return eq_value(g);
}

uint32_t muse_mathpractice_remaining_s(const muse_mathpractice_t *g)
{
    if (g->elapsed_ms >= MUSE_MATHPRACTICE_GAME_MS) {
        return 0;
    }
    return (MUSE_MATHPRACTICE_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

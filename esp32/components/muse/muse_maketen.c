/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_maketen.c -- 凑十练习 logic, see muse_maketen.h.
 *
 * Each question is one make-ten addition, dealt by next_question():
 * a in 6..9, b in 3..9, a + b in 11..18, and b larger than the
 * complement c = 10 - a (so the remainder r = b - c is at least 1).
 * The correct solution is a + c + r = 10 + r. A wrong solution is
 * one of two kinds, picked at random: a wrong split (c' is not the
 * complement of a, c' + r' still equals b, so the chain reads
 * plausibly but the whole equation is false) or a wrong final sum
 * (the split is right, the displayed 10 + r' is off by one). The
 * 1:1 correct/wrong split is a per-question coin flip (user rule
 * 2026-11-08), so a round averages 1:1.
 *
 * A judgement only ever adds: right is +1, wrong is +0. The round
 * ends only on the 30 s timer -- there is no early end and no lives.
 */
#include "muse_maketen.h"

#include <stdlib.h>

/* A wrong split: a first part c' that is not the complement of a
 * (so the split does not make ten with a), still smaller than b so
 * the second part r' = b - c' stays >= 1 and the displayed chain
 * reads plausibly. Returns c' through the out parameters. There is
 * always at least one candidate: b >= 3, and the values 1..b-1
 * except c are non-empty for every (a, b) the dealer can produce
 * (a == 9, c == 1 leaves 2..b-1; any other a leaves 1 in range). */static void wrong_split(const muse_maketen_t *g, int *out_c, int *out_r)
{
    /* Candidates are the values 1..b-1 except the true complement.
     * Non-empty by construction (see the header comment above): b >= 3
     * and the dealer only produces (a, b) with b > c. */
    int cands[9];
    int n = 0;
    for (int v = 1; v <= MUSE_MAKETEN_B_MAX; v++) {
        if (v != g->c && v < g->b) {
            cands[n++] = v;
        }
    }
    int c = cands[rand() % n];
    *out_c = c;
    *out_r = g->b - c;
}

/* A wrong final sum: the true remainder shifted by one, staying
 * inside 0..9 and never equal to the true remainder. r is 1..8, so
 * at least one of r+1 / r-1 is always valid. */
static int wrong_sum(const muse_maketen_t *g)
{
    int r = g->r;
    int d = (rand() % 2 == 0) ? 1 : -1;
    int v = 10 + r + d;
    if (v < 10 || v > 19) {
        v = 10 + r - d;
    }
    return v;
}

static void next_question(muse_maketen_t *g)
{
    /* Deal a make-ten question: a in 6..9, b in 3..9, a + b in
     * 11..18, b > complement of a (remainder at least 1). The loop
     * retries until the constraints hold; a and b are independent
     * draws, so it terminates quickly. */
    for (;;) {
        g->a = MUSE_MAKETEN_A_MIN +
               rand() % (MUSE_MAKETEN_A_MAX - MUSE_MAKETEN_A_MIN + 1);
        g->b = MUSE_MAKETEN_B_MIN +
               rand() % (MUSE_MAKETEN_B_MAX - MUSE_MAKETEN_B_MIN + 1);
        g->c = 10 - g->a;
        if (g->a + g->b >= 11 && g->a + g->b <= MUSE_MAKETEN_SUM_MAX &&
            g->b > g->c) {
            break;
        }
    }
    g->r = g->b - g->c;
    /* 1:1 correct/wrong (user rule 2026-11-08): a coin flip per
     * question, so a round averages half right, half wrong. */
    if (rand() % 2 == 0) {
        g->shown_c = g->c;
        g->shown_r = g->r;
        g->shown_sum = 10 + g->r;
        g->is_correct = true;
    } else {
        g->is_correct = false;
        if (rand() % 2 == 0) {
            /* Wrong split: the first part is not the complement. */
            wrong_split(g, &g->shown_c, &g->shown_r);
            g->shown_sum = 10 + g->shown_r;
        } else {
            /* Wrong final sum: the split is right, the sum is off. */
            g->shown_c = g->c;
            g->shown_r = g->r;
            g->shown_sum = wrong_sum(g);
        }
    }
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

bool muse_maketen_answer(muse_maketen_t *g, bool judged_correct)
{
    if (g->phase != MUSE_MAKETEN_PLAYING) {
        return false;
    }
    bool right = (judged_correct == g->is_correct);
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

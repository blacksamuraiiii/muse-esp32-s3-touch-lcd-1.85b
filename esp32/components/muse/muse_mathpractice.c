/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_mathpractice.c -- 加减练习 logic, see muse_mathpractice.h.
 *
 * Each question is one equation within 20, dealt by next_question():
 * addition picks a in 0..20 and b in 0..20-a (so a+b <= 20),
 * subtraction picks a in 0..20 and b in 0..a (so a-b >= 0). The
 * displayed answer is the true one half the time and a plausible
 * near-miss the other half (user rule 2026-11-08, 1:1): off by 1, 2
 * or 10, staying inside 0..20, never equal to the true answer. The
 * 1:1 split is a per-question coin flip, so a round averages 1:1.
 *
 * A judgement only ever adds: right is +1, wrong is +0. The round
 * ends only on the 30 s timer -- there is no early end and no lives.
 */
#include "muse_mathpractice.h"

#include <stdlib.h>

/* A plausible wrong answer: the true one shifted by a small delta,
 * staying inside 0..20 and never equal to the true answer. The deltas
 * are tried from a random starting offset so no single offset
 * dominates; at least one of +1/-1 is always valid for 0..20. */
static int wrong_answer(int true_ans)
{
    static const int deltas[] = {1, -1, 2, -2, 10, -10};
    int n = (int)(sizeof(deltas) / sizeof(deltas[0]));
    int start = rand() % n;
    for (int i = 0; i < n; i++) {
        int v = true_ans + deltas[(start + i) % n];
        if (v >= 0 && v <= MUSE_MATHPRACTICE_MAX_NUM && v != true_ans) {
            return v;
        }
    }
    return true_ans == 0 ? 1 : true_ans - 1;
}

static void next_question(muse_mathpractice_t *g)
{
    g->is_add = (rand() % 2 == 0);
    if (g->is_add) {
        g->a = rand() % (MUSE_MATHPRACTICE_MAX_NUM + 1);
        g->b = rand() % (MUSE_MATHPRACTICE_MAX_NUM + 1 - g->a);
    } else {
        g->a = rand() % (MUSE_MATHPRACTICE_MAX_NUM + 1);
        g->b = rand() % (g->a + 1);
    }
    int true_ans = muse_mathpractice_true_answer(g);
    /* 1:1 correct/wrong (user rule 2026-11-08): a coin flip per
     * question, so a round averages half right, half wrong. */
    if (rand() % 2 == 0) {
        g->shown = true_ans;
        g->is_correct = true;
    } else {
        g->shown = wrong_answer(true_ans);
        g->is_correct = false;
    }
}

static void reset_game(muse_mathpractice_t *g)
{
    g->score = 0;
    g->answered = 0;
    g->elapsed_ms = 0;
    next_question(g);
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
    }
}

bool muse_mathpractice_answer(muse_mathpractice_t *g, bool judged_correct)
{
    if (g->phase != MUSE_MATHPRACTICE_PLAYING) {
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

int muse_mathpractice_true_answer(const muse_mathpractice_t *g)
{
    return g->is_add ? g->a + g->b : g->a - g->b;
}

uint32_t muse_mathpractice_remaining_s(const muse_mathpractice_t *g)
{
    if (g->elapsed_ms >= MUSE_MATHPRACTICE_GAME_MS) {
        return 0;
    }
    return (MUSE_MATHPRACTICE_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

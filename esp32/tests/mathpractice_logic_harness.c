/* 加减练习 logic harness: checks muse_mathpractice.c without LVGL
 * or ESP-IDF. Run by tests/test_mathpractice_logic.py. Prints "ok"
 * on success; any failed check aborts via assert. Deterministic: the
 * random picks are seeded once with srand().
 *
 * The rules under test are the ones the user settled on 2026-11-08:
 * 20以内加减法, the displayed answer is right about half the time
 * and wrong about half the time (1:1), a correct judgement is +1
 * and a wrong one is +0 (never negative), and the round is 30 s. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_mathpractice.h"

static void tick_n(muse_mathpractice_t *g, int n)
{
    for (int i = 0; i < n; i++) {
        muse_mathpractice_tick(g, MUSE_MATHPRACTICE_TICK_MS);
    }
}

/* The equation the game is currently showing must always be legal:
 * operands in 0..20, the true answer in 0..20, the displayed answer
 * in 0..20, and is_correct exactly when shown == true. */
static void check_question(const muse_mathpractice_t *g)
{
    assert(g->a >= 0 && g->a <= MUSE_MATHPRACTICE_MAX_NUM);
    assert(g->b >= 0 && g->b <= MUSE_MATHPRACTICE_MAX_NUM);
    int true_ans = muse_mathpractice_true_answer(g);
    assert(true_ans >= 0 && true_ans <= MUSE_MATHPRACTICE_MAX_NUM);
    if (g->is_add) {
        assert(g->a + g->b <= MUSE_MATHPRACTICE_MAX_NUM);
    } else {
        assert(g->a >= g->b);
    }
    assert(g->shown >= 0 && g->shown <= MUSE_MATHPRACTICE_MAX_NUM);
    assert(g->is_correct == (g->shown == true_ans));
    /* A wrong displayed answer is a near-miss, never an absurd value. */
    if (!g->is_correct) {
        int d = g->shown - true_ans;
        if (d < 0) {
            d = -d;
        }
        assert(d == 1 || d == 2 || d == 10);
    }
}

int main(void)
{
    srand(12345);

    /* Init state: START, no score, a legal first equation. */
    muse_mathpractice_t g;
    muse_mathpractice_init(&g);
    assert(g.phase == MUSE_MATHPRACTICE_START);
    assert(g.score == 0 && g.answered == 0 && g.elapsed_ms == 0);
    check_question(&g);
    assert(muse_mathpractice_remaining_s(&g) == 30);

    /* Phase guards before start: pause / resume / tick / answer are
     * all no-ops, the equation does not change. */
    int a0 = g.a, b0 = g.b, shown0 = g.shown;
    muse_mathpractice_pause(&g);
    assert(g.phase == MUSE_MATHPRACTICE_START);
    muse_mathpractice_resume(&g);
    assert(g.phase == MUSE_MATHPRACTICE_START);
    muse_mathpractice_tick(&g, MUSE_MATHPRACTICE_TICK_MS);
    assert(g.elapsed_ms == 0);
    assert(!muse_mathpractice_answer(&g, true));
    assert(g.score == 0 && g.answered == 0);
    assert(g.a == a0 && g.b == b0 && g.shown == shown0);

    /* Start: playing, fresh score, a legal equation. */
    muse_mathpractice_start(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    assert(g.score == 0 && g.answered == 0);
    check_question(&g);
    muse_mathpractice_start(&g); /* no-op from PLAYING */
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);

    /* Answering: judging a right equation right is +1, judging it
     * wrong is +0; judging a wrong equation wrong is +1, judging it
     * right is +0. Either way the next equation is dealt at once. */
    for (int i = 0; i < 200; i++) {
        check_question(&g);
        bool is_correct = g.is_correct;
        uint32_t score0 = g.score;
        uint32_t answered0 = g.answered;
        /* Judge it right: scores iff it really is right. */
        bool got = muse_mathpractice_answer(&g, true);
        assert(got == is_correct);
        assert(g.answered == answered0 + 1);
        assert(g.score == score0 + (is_correct ? 1u : 0u));
        check_question(&g);
        /* Judge the next one wrong: scores iff it really is wrong. */
        is_correct = g.is_correct;
        score0 = g.score;
        answered0 = g.answered;
        got = muse_mathpractice_answer(&g, false);
        assert(got == !is_correct);
        assert(g.answered == answered0 + 1);
        assert(g.score == score0 + (is_correct ? 0u : 1u));
        check_question(&g);
    }

    /* 1:1 correct/wrong (user rule 2026-11-08): over 2000 fresh
     * questions the split must sit near half -- a coin flip, not a
     * fixed pattern and not skewed. */
    muse_mathpractice_restart(&g);
    int right_n = 0;
    int total_n = 2000;
    for (int i = 0; i < total_n; i++) {
        check_question(&g);
        if (g.is_correct) {
            right_n++;
        }
        /* Answer correctly every time, so the game keeps dealing. */
        muse_mathpractice_answer(&g, g.is_correct);
    }
    assert(right_n > total_n * 40 / 100 && right_n < total_n * 60 / 100);

    /* Both operations appear: addition and subtraction both show up
     * over a long run. */
    muse_mathpractice_restart(&g);
    int add_n = 0;
    for (int i = 0; i < total_n; i++) {
        if (g.is_add) {
            add_n++;
        }
        muse_mathpractice_answer(&g, g.is_correct);
    }
    assert(add_n > total_n * 40 / 100 && add_n < total_n * 60 / 100);

    /* Timer: 30 s ends the round. remaining_s counts down. */
    muse_mathpractice_restart(&g);
    assert(muse_mathpractice_remaining_s(&g) == 30);
    tick_n(&g, 20); /* 1 s */
    assert(muse_mathpractice_remaining_s(&g) == 29);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    tick_n(&g, 580); /* 29 s more: 30 s total */
    assert(g.phase == MUSE_MATHPRACTICE_OVER);
    assert(muse_mathpractice_remaining_s(&g) == 0);
    /* Answering after OVER is a no-op. */
    uint32_t score0 = g.score;
    assert(!muse_mathpractice_answer(&g, true));
    assert(g.score == score0);

    /* Pause freezes the timer; resume continues. */
    muse_mathpractice_restart(&g);
    tick_n(&g, 20); /* 1 s */
    uint32_t elapsed0 = g.elapsed_ms;
    muse_mathpractice_pause(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PAUSED);
    tick_n(&g, 20);
    assert(g.elapsed_ms == elapsed0);
    muse_mathpractice_resume(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    tick_n(&g, 20);
    assert(g.elapsed_ms > elapsed0);

    /* Restart after OVER: fresh game, fee is the UI's job. */
    tick_n(&g, 600); /* run the clock out */
    assert(g.phase == MUSE_MATHPRACTICE_OVER);
    muse_mathpractice_restart(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    assert(g.score == 0 && g.answered == 0 && g.elapsed_ms == 0);
    check_question(&g);

    printf("ok\n");
    return 0;
}

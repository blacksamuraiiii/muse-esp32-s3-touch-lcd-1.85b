/* 凑十练习 logic harness: checks muse_maketen.c without LVGL
 * or ESP-IDF. Run by tests/test_maketen_logic.py. Prints "ok"
 * on success; any failed check aborts via assert. Deterministic: the
 * random picks are seeded once with srand().
 *
 * The rules under test are the ones the user settled on 2026-11-08:
 * 凑十法 (a in 6..9, b in 3..9, a + b in 11..18, b larger than the
 * complement of a), the displayed solution is right about half the
 * time and wrong about half the time (1:1), a correct judgement is
 * +1 and a wrong one is +0 (never negative), and the round is 30 s. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_maketen.h"

static void tick_n(muse_maketen_t *g, int n)
{
    for (int i = 0; i < n; i++) {
        muse_maketen_tick(g, MUSE_MAKETEN_TICK_MS);
    }
}

/* The question the game is currently showing must always be legal:
 * a in 6..9, b in 3..9, a + b in 11..18, c = 10 - a, r = b - c >= 1,
 * the displayed split always adds up to b (shown_c + shown_r == b),
 * and is_correct exactly when the displayed solution is the true
 * one (shown_c == c, shown_r == r, shown_sum == 10 + r). */
static void check_question(const muse_maketen_t *g)
{
    assert(g->a >= MUSE_MAKETEN_A_MIN && g->a <= MUSE_MAKETEN_A_MAX);
    assert(g->b >= MUSE_MAKETEN_B_MIN && g->b <= MUSE_MAKETEN_B_MAX);
    assert(g->a + g->b >= 11 && g->a + g->b <= MUSE_MAKETEN_SUM_MAX);
    assert(g->c == 10 - g->a);
    assert(g->r == g->b - g->c);
    assert(g->r >= 1);
    int true_ans = muse_maketen_true_answer(g);
    assert(true_ans == g->a + g->b);
    /* The displayed split always adds up to b -- even a wrong split
     * reads plausibly, so the player must check the complement. */
    assert(g->shown_c + g->shown_r == g->b);
    assert(g->shown_c >= 1 && g->shown_c <= 9);
    assert(g->shown_r >= 1 && g->shown_r <= 9);
    assert(g->shown_sum >= 10 && g->shown_sum <= 19);
    bool true_solution = (g->shown_c == g->c && g->shown_r == g->r &&
                          g->shown_sum == 10 + g->r);
    assert(g->is_correct == true_solution);
    if (!g->is_correct) {
        /* A wrong solution is one of two kinds: a wrong split (the
         * first part is not the complement, the sum follows the wrong
         * split) or a wrong final sum (the split is right, the sum is
         * off by one). */
        bool wrong_split = (g->shown_c != g->c &&
                            g->shown_sum == 10 + g->shown_r);
        bool wrong_sum = (g->shown_c == g->c && g->shown_r == g->r &&
                          g->shown_sum != 10 + g->r);
        assert(wrong_split || wrong_sum);
        if (wrong_sum) {
            int d = g->shown_sum - (10 + g->r);
            assert(d == 1 || d == -1);
        }
    }
}

int main(void)
{
    srand(12345);

    /* Init state: START, no score, a legal first question. */
    muse_maketen_t g;
    muse_maketen_init(&g);
    assert(g.phase == MUSE_MAKETEN_START);
    assert(g.score == 0 && g.answered == 0 && g.elapsed_ms == 0);
    check_question(&g);
    assert(muse_maketen_remaining_s(&g) == 30);

    /* Phase guards before start: pause / resume / tick / answer are
     * all no-ops, the question does not change. */
    int a0 = g.a, b0 = g.b, sc0 = g.shown_c, sr0 = g.shown_r;
    muse_maketen_pause(&g);
    assert(g.phase == MUSE_MAKETEN_START);
    muse_maketen_resume(&g);
    assert(g.phase == MUSE_MAKETEN_START);
    muse_maketen_tick(&g, MUSE_MAKETEN_TICK_MS);
    assert(g.elapsed_ms == 0);
    assert(!muse_maketen_answer(&g, true));
    assert(g.score == 0 && g.answered == 0);
    assert(g.a == a0 && g.b == b0 && g.shown_c == sc0 && g.shown_r == sr0);

    /* Start: playing, fresh score, a legal question. */
    muse_maketen_start(&g);
    assert(g.phase == MUSE_MAKETEN_PLAYING);
    assert(g.score == 0 && g.answered == 0);
    check_question(&g);
    muse_maketen_start(&g); /* no-op from PLAYING */
    assert(g.phase == MUSE_MAKETEN_PLAYING);

    /* Answering: judging a right solution right is +1, judging it
     * wrong is +0; judging a wrong solution wrong is +1, judging it
     * right is +0. Either way the next question is dealt at once. */
    for (int i = 0; i < 200; i++) {
        check_question(&g);
        bool is_correct = g.is_correct;
        uint32_t score0 = g.score;
        uint32_t answered0 = g.answered;
        /* Judge it right: scores iff it really is right. */
        bool got = muse_maketen_answer(&g, true);
        assert(got == is_correct);
        assert(g.answered == answered0 + 1);
        assert(g.score == score0 + (is_correct ? 1u : 0u));
        check_question(&g);
        /* Judge the next one wrong: scores iff it really is wrong. */
        is_correct = g.is_correct;
        score0 = g.score;
        answered0 = g.answered;
        got = muse_maketen_answer(&g, false);
        assert(got == !is_correct);
        assert(g.answered == answered0 + 1);
        assert(g.score == score0 + (is_correct ? 0u : 1u));
        check_question(&g);
    }

    /* 1:1 correct/wrong (user rule 2026-11-08): over 2000 fresh
     * questions the split must sit near half -- a coin flip, not a
     * fixed pattern and not skewed. */
    muse_maketen_restart(&g);
    int right_n = 0;
    int total_n = 2000;
    for (int i = 0; i < total_n; i++) {
        check_question(&g);
        if (g.is_correct) {
            right_n++;
        }
        /* Answer correctly every time, so the game keeps dealing. */
        muse_maketen_answer(&g, g.is_correct);
    }
    assert(right_n > total_n * 40 / 100 && right_n < total_n * 60 / 100);

    /* Both wrong kinds appear over a long run: wrong splits and
     * wrong final sums. */
    muse_maketen_restart(&g);
    int wrong_split_n = 0, wrong_sum_n = 0;
    for (int i = 0; i < total_n; i++) {
        if (!g.is_correct) {
            if (g.shown_c != g.c) {
                wrong_split_n++;
            } else {
                wrong_sum_n++;
            }
        }
        muse_maketen_answer(&g, g.is_correct);
    }
    assert(wrong_split_n > 0 && wrong_sum_n > 0);

    /* Timer: 30 s ends the round. remaining_s counts down. */
    muse_maketen_restart(&g);
    assert(muse_maketen_remaining_s(&g) == 30);
    tick_n(&g, 20); /* 1 s */
    assert(muse_maketen_remaining_s(&g) == 29);
    assert(g.phase == MUSE_MAKETEN_PLAYING);
    tick_n(&g, 580); /* 29 s more: 30 s total */
    assert(g.phase == MUSE_MAKETEN_OVER);
    assert(muse_maketen_remaining_s(&g) == 0);
    /* Answering after OVER is a no-op. */
    uint32_t score0 = g.score;
    assert(!muse_maketen_answer(&g, true));
    assert(g.score == score0);

    /* Pause freezes the timer; resume continues. */
    muse_maketen_restart(&g);
    tick_n(&g, 20); /* 1 s */
    uint32_t elapsed0 = g.elapsed_ms;
    muse_maketen_pause(&g);
    assert(g.phase == MUSE_MAKETEN_PAUSED);
    tick_n(&g, 20);
    assert(g.elapsed_ms == elapsed0);
    muse_maketen_resume(&g);
    assert(g.phase == MUSE_MAKETEN_PLAYING);
    tick_n(&g, 20);
    assert(g.elapsed_ms > elapsed0);

    /* Restart after OVER: fresh game, fee is the UI's job. */
    tick_n(&g, 600); /* run the clock out */
    assert(g.phase == MUSE_MAKETEN_OVER);
    muse_maketen_restart(&g);
    assert(g.phase == MUSE_MAKETEN_PLAYING);
    assert(g.score == 0 && g.answered == 0 && g.elapsed_ms == 0);
    check_question(&g);

    printf("ok\n");
    return 0;
}

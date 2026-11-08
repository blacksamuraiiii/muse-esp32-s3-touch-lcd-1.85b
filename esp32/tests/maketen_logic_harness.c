/* 凑十法练习 logic harness: checks muse_maketen.c without LVGL
 * or ESP-IDF. Run by tests/test_maketen_logic.py. Prints "ok"
 * on success; any failed check aborts via assert. Deterministic: the
 * random picks are seeded once with srand().
 *
 * The rules under test are the ones the user settled on 2026-11-08:
 * 比较三选一 (a + b ? d with >, < or =), a, b in 2..9, a + b in
 * 11..18, the comparison number d is the true sum about a third of
 * the time and a near-miss (±1 or ±2) the rest, the hint is the two
 * make-ten steps as subtractions (10 - big = need, small - need =
 * left), a correct pick is +1 and a wrong one is +0 (never
 * negative), and the round is 30 s. */
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
 * a, b in 2..9, a + b in 11..18, d in 0..20, the correct comparison
 * matches the sign of (a + b) - d, and the hint is the two make-ten
 * steps as subtractions: hint_big is the larger addend (6..9),
 * hint_small the smaller, hint_need = 10 - hint_big (1..4) is what
 * big is short of 10, and hint_left = hint_small - hint_need (>= 1)
 * is what remains of small after giving need to big. The UI prints
 * "10-big=need  small-need=left". */
static void check_question(const muse_maketen_t *g)
{
    assert(g->a >= MUSE_MAKETEN_A_MIN && g->a <= MUSE_MAKETEN_A_MAX);
    assert(g->b >= MUSE_MAKETEN_B_MIN && g->b <= MUSE_MAKETEN_B_MAX);
    int sum = muse_maketen_true_answer(g);
    assert(sum == g->a + g->b);
    assert(sum >= MUSE_MAKETEN_SUM_MIN && sum <= MUSE_MAKETEN_SUM_MAX);
    assert(g->d >= 0 && g->d <= MUSE_MAKETEN_MAX_NUM);
    if (sum > g->d) {
        assert(g->correct == MUSE_MAKETEN_CMP_GT);
    } else if (sum < g->d) {
        assert(g->correct == MUSE_MAKETEN_CMP_LT);
    } else {
        assert(g->correct == MUSE_MAKETEN_CMP_EQ);
    }
    /* The comparison number is the true sum or a near-miss (±1, ±2),
     * never an absurd value. */
    int diff = g->d - sum;
    if (diff < 0) {
        diff = -diff;
    }
    assert(diff == 0 || diff == 1 || diff == 2);
    /* The hint is the two make-ten steps, as subtractions. */
    int big = g->a > g->b ? g->a : g->b;
    int small = g->a > g->b ? g->b : g->a;
    assert(g->hint_big == big);
    assert(g->hint_small == small);
    assert(g->hint_big >= 6 && g->hint_big <= 9);
    assert(g->hint_need == 10 - g->hint_big);
    assert(g->hint_need >= 1 && g->hint_need <= 4);
    assert(g->hint_left == g->hint_small - g->hint_need);
    assert(g->hint_left >= 1);
    /* The steps recombine to the sum: big + need is 10, and
     * 10 + left is the true sum. */
    assert(g->hint_big + g->hint_need == 10);
    assert(10 + g->hint_left == sum);
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
    int a0 = g.a, b0 = g.b, d0 = g.d;
    muse_maketen_pause(&g);
    assert(g.phase == MUSE_MAKETEN_START);
    muse_maketen_resume(&g);
    assert(g.phase == MUSE_MAKETEN_START);
    muse_maketen_tick(&g, MUSE_MAKETEN_TICK_MS);
    assert(g.elapsed_ms == 0);
    assert(!muse_maketen_answer(&g, MUSE_MAKETEN_CMP_EQ));
    assert(g.score == 0 && g.answered == 0);
    assert(g.a == a0 && g.b == b0 && g.d == d0);

    /* Start: playing, fresh score, a legal question. */
    muse_maketen_start(&g);
    assert(g.phase == MUSE_MAKETEN_PLAYING);
    assert(g.score == 0 && g.answered == 0);
    check_question(&g);
    muse_maketen_start(&g); /* no-op from PLAYING */
    assert(g.phase == MUSE_MAKETEN_PLAYING);

    /* Answering: picking the right comparison is +1, picking a wrong
     * one is +0. Either way the next question is dealt at once. */
    for (int i = 0; i < 200; i++) {
        check_question(&g);
        muse_maketen_cmp_t correct = g.correct;
        uint32_t score0 = g.score;
        uint32_t answered0 = g.answered;
        /* Pick the right one: scores. */
        bool got = muse_maketen_answer(&g, correct);
        assert(got);
        assert(g.answered == answered0 + 1);
        assert(g.score == score0 + 1);
        check_question(&g);
        /* Pick a wrong one: no score. */
        correct = g.correct;
        score0 = g.score;
        answered0 = g.answered;
        muse_maketen_cmp_t wrong = (correct == MUSE_MAKETEN_CMP_GT)
                                       ? MUSE_MAKETEN_CMP_LT
                                       : MUSE_MAKETEN_CMP_GT;
        got = muse_maketen_answer(&g, wrong);
        assert(!got);
        assert(g.answered == answered0 + 1);
        assert(g.score == score0);
        check_question(&g);
    }

    /* The three answers all appear: = (d == sum) about a third of
     * the time, > and < the rest. Over 2000 fresh questions each
     * must sit in a sane band -- a three-way split, not a fixed
     * pattern and not skewed. */
    muse_maketen_restart(&g);
    int eq_n = 0, gt_n = 0, lt_n = 0;
    int total_n = 2000;
    for (int i = 0; i < total_n; i++) {
        check_question(&g);
        if (g.correct == MUSE_MAKETEN_CMP_EQ) {
            eq_n++;
        } else if (g.correct == MUSE_MAKETEN_CMP_GT) {
            gt_n++;
        } else {
            lt_n++;
        }
        /* Answer correctly every time, so the game keeps dealing. */
        muse_maketen_answer(&g, g.correct);
    }
    assert(eq_n > total_n * 25 / 100 && eq_n < total_n * 42 / 100);
    assert(gt_n > total_n * 25 / 100 && gt_n < total_n * 42 / 100);
    assert(lt_n > total_n * 25 / 100 && lt_n < total_n * 42 / 100);

    /* The hint's big addend is sometimes a, sometimes b: the dealer
     * draws a and b independently, so over a long run both orderings
     * appear (the hint itself is deterministic given a and b -- big
     * is always the larger addend). */
    muse_maketen_restart(&g);
    int big_a_n = 0, big_b_n = 0;
    for (int i = 0; i < total_n; i++) {
        if (g.hint_big == g.a) {
            big_a_n++;
        } else {
            big_b_n++;
        }
        muse_maketen_answer(&g, g.correct);
    }
    assert(big_a_n > 0 && big_b_n > 0);

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
    assert(!muse_maketen_answer(&g, MUSE_MAKETEN_CMP_EQ));
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

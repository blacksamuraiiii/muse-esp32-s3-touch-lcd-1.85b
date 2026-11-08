/* 凑十法练习 logic harness: checks muse_maketen.c without LVGL
 * or ESP-IDF. Run by tests/test_maketen_logic.py. Prints "ok"
 * on success; any failed check aborts via assert. Deterministic: the
 * random picks are seeded once with srand().
 *
 * The rules under test are the ones the user settled on 2026-11-08:
 * 比较三选一 (a + b ? d with >, < or =), a, b in 2..9, a + b in
 * 11..18, the comparison number d is the true sum about a third of
 * the time and a near-miss (±1 or ±2) the rest, the hint is a valid
 * make-ten split of one addend, a correct pick is +1 and a wrong
 * one is +0 (never negative), and the round is 30 s. */
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
 * matches the sign of (a + b) - d, and the hint is a valid make-ten
 * split: hint_y + hint_z == hint_x, hint_x is one of the addends,
 * and the split makes ten with the other addend
 * (hint_y == 10 - other). */
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
    /* The hint is a valid make-ten split of one addend. */
    assert(g->hint_y + g->hint_z == g->hint_x);
    assert(g->hint_y >= 1 && g->hint_z >= 1);
    if (g->hint_x == g->a) {
        assert(g->hint_y == 10 - g->b);
    } else {
        assert(g->hint_x == g->b);
        assert(g->hint_y == 10 - g->a);
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

    /* The hint splits either addend over a long run. */
    muse_maketen_restart(&g);
    int split_a_n = 0, split_b_n = 0;
    for (int i = 0; i < total_n; i++) {
        if (g.hint_x == g.a) {
            split_a_n++;
        } else {
            split_b_n++;
        }
        muse_maketen_answer(&g, g.correct);
    }
    assert(split_a_n > 0 && split_b_n > 0);

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

/* 加减练习 logic harness: checks muse_mathpractice.c without LVGL
 * or ESP-IDF. Run by tests/test_mathpractice_logic.py. Prints "ok"
 * on success; any failed check aborts via assert. Deterministic: the
 * random picks are seeded once with srand().
 *
 * The rules under test are the ones the user settled on 2026-11-08
 * (the round that fused 凑十法练习 into this game):
 *
 *   - two kinds of question dealt 1:1: 个位数相加 (a, b in 2..9,
 *     11 <= a + b <= 18, so 凑十法 applies) and 十几减个位
 *     (minuend 11..18, subtrahend 2..9 and larger than the ones
 *     digit, so the subtraction borrows ten);
 *   - the comparison number d is the true value or a near-miss of
 *     +-1 / +-2, and `correct` is exactly the sign of value - d;
 *   - a right pick is +1, a wrong one +0 (never negative);
 *   - the verdict holds for FEEDBACK_MS: no new question, no answer
 *     taken, then the next question lands;
 *   - the round is 2 minutes (MUSE_MATHPRACTICE_GAME_MS).
 */
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

/* The question on screen must always be legal. */
static void check_question(const muse_mathpractice_t *g)
{
    int value = muse_mathpractice_value(g);
    if (g->kind == MUSE_MATHPRACTICE_KIND_ADD_TEN) {
        /* 个位数相加，和要进位（凑十法的适用范围）。 */
        assert(g->a >= MUSE_MATHPRACTICE_ADDEND_MIN &&
               g->a <= MUSE_MATHPRACTICE_ADDEND_MAX);
        assert(g->b >= MUSE_MATHPRACTICE_ADDEND_MIN &&
               g->b <= MUSE_MATHPRACTICE_ADDEND_MAX);
        assert(value >= MUSE_MATHPRACTICE_SUM_MIN &&
               value <= MUSE_MATHPRACTICE_SUM_MAX);
        assert(value == g->a + g->b);
    } else {
        /* 十几减个位，必须借位：减数大于被减数的个位。 */
        assert(g->a >= MUSE_MATHPRACTICE_MINUEND_MIN &&
               g->a <= MUSE_MATHPRACTICE_MINUEND_MAX);
        assert(g->b >= MUSE_MATHPRACTICE_ADDEND_MIN &&
               g->b <= MUSE_MATHPRACTICE_ADDEND_MAX);
        assert(g->b > g->a % 10);
        assert(value == g->a - g->b);
        assert(value >= 2 && value <= 9);
    }
    /* 比较数在 0..20，与真值最多差 2，且符号判断一致。 */
    assert(g->d >= 0 && g->d <= MUSE_MATHPRACTICE_MAX_NUM);
    int diff = g->d - value;
    if (diff < 0) {
        diff = -diff;
    }
    assert(diff == 0 || diff == 1 || diff == 2);
    muse_mathpractice_cmp_t want = value > g->d ? MUSE_MATHPRACTICE_CMP_GT
                                  : value < g->d ? MUSE_MATHPRACTICE_CMP_LT
                                                 : MUSE_MATHPRACTICE_CMP_EQ;
    assert(g->correct == want);
}

/* Answer the current question correctly and run the verdict window
 * out, so the next question is dealt. */
static void answer_right_and_wait(muse_mathpractice_t *g)
{
    assert(muse_mathpractice_answer(g, g->correct));
    assert(g->feedback_ms == MUSE_MATHPRACTICE_FEEDBACK_MS);
    tick_n(g, (int)(MUSE_MATHPRACTICE_FEEDBACK_MS / MUSE_MATHPRACTICE_TICK_MS));
    assert(g->feedback_ms == 0);
}

int main(void)
{
    srand(12345);

    /* Init state: START, no score, a legal first question. */
    muse_mathpractice_t g;
    muse_mathpractice_init(&g);
    assert(g.phase == MUSE_MATHPRACTICE_START);
    assert(g.score == 0 && g.answered == 0 && g.elapsed_ms == 0);
    assert(g.feedback_ms == 0);
    check_question(&g);
    assert(muse_mathpractice_remaining_s(&g) == MUSE_MATHPRACTICE_GAME_MS / 1000);

    /* Phase guards before start: pause / resume / tick / answer are
     * all no-ops, the question does not change. */
    int a0 = g.a, b0 = g.b, d0 = g.d;
    muse_mathpractice_pause(&g);
    assert(g.phase == MUSE_MATHPRACTICE_START);
    muse_mathpractice_resume(&g);
    assert(g.phase == MUSE_MATHPRACTICE_START);
    muse_mathpractice_tick(&g, MUSE_MATHPRACTICE_TICK_MS);
    assert(g.elapsed_ms == 0);
    assert(!muse_mathpractice_answer(&g, MUSE_MATHPRACTICE_CMP_EQ));
    assert(g.score == 0 && g.answered == 0);
    assert(g.a == a0 && g.b == b0 && g.d == d0);

    /* Start: playing, fresh score, a legal question. */
    muse_mathpractice_start(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    assert(g.score == 0 && g.answered == 0);
    check_question(&g);
    muse_mathpractice_start(&g); /* no-op from PLAYING */
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);

    /* Grading and the verdict window, over 10 short rounds (each
     * round is restarted so the 3-minute clock never ends it early):
     *   - a right pick is +1 and answered +1;
     *   - the verdict holds for the whole window: another tap is
     *     ignored (score and answered unchanged) and the question
     *     does not change;
     *   - the next question lands when the window expires. */
    for (int round = 0; round < 10; round++) {
        muse_mathpractice_restart(&g);
        for (int i = 0; i < 20; i++) {
            check_question(&g);
            int a_before = g.a, b_before = g.b, d_before = g.d;
            uint32_t score0 = g.score;
            uint32_t answered0 = g.answered;
            assert(muse_mathpractice_answer(&g, g.correct));
            assert(g.score == score0 + 1);
            assert(g.answered == answered0 + 1);
            assert(g.feedback_ms == MUSE_MATHPRACTICE_FEEDBACK_MS);
            assert(g.last_right);
            /* A tap during the window is ignored: nothing moves, and
             * especially no question is skipped. */
            assert(!muse_mathpractice_answer(&g, g.correct));
            assert(g.score == score0 + 1 && g.answered == answered0 + 1);
            assert(g.a == a_before && g.b == b_before && g.d == d_before);
            /* One tick short of the window: still the same question. */
            tick_n(&g, (int)(MUSE_MATHPRACTICE_FEEDBACK_MS /
                             MUSE_MATHPRACTICE_TICK_MS) - 1);
            assert(g.feedback_ms == MUSE_MATHPRACTICE_TICK_MS);
            assert(g.a == a_before && g.b == b_before && g.d == d_before);
            /* The last tick deals the next question. */
            tick_n(&g, 1);
            assert(g.feedback_ms == 0);
            check_question(&g);
        }

        /* A wrong pick is +0, answered +1, and still shows a
         * verdict (last_right false). */
        muse_mathpractice_restart(&g);
        for (int i = 0; i < 10; i++) {
            check_question(&g);
            uint32_t score0 = g.score;
            uint32_t answered0 = g.answered;
            muse_mathpractice_cmp_t wrong =
                (g.correct == MUSE_MATHPRACTICE_CMP_GT)
                    ? MUSE_MATHPRACTICE_CMP_LT
                    : MUSE_MATHPRACTICE_CMP_GT;
            assert(!muse_mathpractice_answer(&g, wrong));
            assert(g.score == score0);
            assert(g.answered == answered0 + 1);
            assert(g.feedback_ms == MUSE_MATHPRACTICE_FEEDBACK_MS);
            assert(!g.last_right);
            tick_n(&g, (int)(MUSE_MATHPRACTICE_FEEDBACK_MS / MUSE_MATHPRACTICE_TICK_MS));
            assert(g.feedback_ms == 0);
        }
    }

    /* The two kinds are dealt 1:1 and every kind stays legal over a
     * long run; the comparison split is 20% equal, 40% each way. */
    int total_n = 2000;
    int add_n = 0, eq_n = 0, gt_n = 0, lt_n = 0;
    for (int round = 0; round < 100; round++) {
        muse_mathpractice_restart(&g);
        for (int i = 0; i < total_n / 100; i++) {
            check_question(&g);
            if (g.kind == MUSE_MATHPRACTICE_KIND_ADD_TEN) {
                add_n++;
            }
            if (g.correct == MUSE_MATHPRACTICE_CMP_EQ) {
                eq_n++;
            } else if (g.correct == MUSE_MATHPRACTICE_CMP_GT) {
                gt_n++;
            } else {
                lt_n++;
            }
            answer_right_and_wait(&g);
        }
    }
    assert(add_n > total_n * 40 / 100 && add_n < total_n * 60 / 100);
    assert(eq_n > total_n * 10 / 100 && eq_n < total_n * 32 / 100);
    assert(gt_n > total_n * 28 / 100 && gt_n < total_n * 52 / 100);
    assert(lt_n > total_n * 28 / 100 && lt_n < total_n * 52 / 100);

    /* Timer: the round ends at MUSE_MATHPRACTICE_GAME_MS. remaining_s
     * counts down from the whole round in seconds. */
    muse_mathpractice_restart(&g);
    const int round_s = (int)(MUSE_MATHPRACTICE_GAME_MS / 1000u);
    const int round_ticks = (int)(MUSE_MATHPRACTICE_GAME_MS / MUSE_MATHPRACTICE_TICK_MS);
    assert(muse_mathpractice_remaining_s(&g) == (uint32_t)round_s);
    tick_n(&g, 20); /* 1 s */
    assert(muse_mathpractice_remaining_s(&g) == (uint32_t)(round_s - 1));
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    tick_n(&g, round_ticks - 20); /* the rest of the round */
    assert(g.phase == MUSE_MATHPRACTICE_OVER);
    assert(muse_mathpractice_remaining_s(&g) == 0);
    /* Answering after OVER is a no-op. */
    uint32_t score0 = g.score;
    assert(!muse_mathpractice_answer(&g, MUSE_MATHPRACTICE_CMP_EQ));
    assert(g.score == score0);

    /* A verdict still showing when the clock runs out is cleared, so
     * the OVER card cannot show a stale grade. */
    muse_mathpractice_restart(&g);
    assert(muse_mathpractice_answer(&g, g.correct));
    assert(g.feedback_ms == MUSE_MATHPRACTICE_FEEDBACK_MS);
    tick_n(&g, round_ticks); /* the whole round */
    assert(g.phase == MUSE_MATHPRACTICE_OVER);
    assert(g.feedback_ms == 0);

    /* Pause freezes the clock and the verdict window; resume
     * continues both. */
    muse_mathpractice_restart(&g);
    tick_n(&g, 20); /* 1 s */
    uint32_t elapsed0 = g.elapsed_ms;
    muse_mathpractice_pause(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PAUSED);
    tick_n(&g, 20);
    assert(g.elapsed_ms == elapsed0);
    assert(!muse_mathpractice_answer(&g, g.correct)); /* paused: ignored */
    muse_mathpractice_resume(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    tick_n(&g, 20);
    assert(g.elapsed_ms > elapsed0);

    /* Restart after OVER: fresh game, fee is the UI's job. */
    tick_n(&g, round_ticks); /* run the clock out */
    assert(g.phase == MUSE_MATHPRACTICE_OVER);
    muse_mathpractice_restart(&g);
    assert(g.phase == MUSE_MATHPRACTICE_PLAYING);
    assert(g.score == 0 && g.answered == 0 && g.elapsed_ms == 0);
    assert(g.feedback_ms == 0);
    check_question(&g);

    printf("ok\n");
    return 0;
}

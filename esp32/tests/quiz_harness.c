/* Host harness for 英语口语 / 听力练习 (1.5.3a B2, mockup v6).
 * Builds with plain cc on the host (same pattern as
 * wordcards_harness.c):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I components/muse tests/quiz_harness.c \
 *      components/muse/muse_quiz.c components/muse/muse_wordcards.c \
 *      -o /tmp/quiz
 * Covers the pure logic only (muse_quiz.c has no LVGL and no
 * ESP-IDF -- the UI owns the 50 ms timer and the audio tasks):
 *  1. Pool: fruit 4 + animal 8 + things 4 = 16 words, in table order.
 *  2. Deal: 5 distinct answers per round, two distractors per
 *     question that are never the answer and never each other, the
 *     three options hold the answer exactly once, the correct slot
 *     is 0..2 -- checked over 200 deals, so the shuffle is exercised
 *     rather than one lucky draw.
 *  3. Answer + feedback: right is +1, wrong is +0, a pick during the
 *     600 ms verdict is ignored, the next question is dealt when the
 *     verdict ends, the last question's verdict ends the round.
 *  4. Clock: 60 seconds, rounded-up remaining seconds, the clock ends
 *     the round even mid-verdict.
 *  5. Pending audio: set on deal and replay, cleared on demand.
 *  6. Cache bookkeeping: begin -> FETCHING, ready/failed marks, a
 *     mark from an older quiz generation is ignored, all-ready.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_quiz.h"

static void test_pool(void)
{
    assert(muse_quiz_pool_words() == 16);
    static const char *en[16] = {
        "apple", "banana", "cherry", "peach",
        "cat", "dog", "bird", "fish", "bear", "duck", "frog", "fox",
        "balloon", "box", "bag", "robot",
    };
    static const char *zh[16] = {
        "苹果", "香蕉", "樱桃", "桃子",
        "猫", "狗", "鸟", "鱼", "熊", "鸭子", "青蛙", "狐狸",
        "气球", "盒子", "包", "机器人",
    };
    for (int i = 0; i < 16; i++) {
        const muse_wordcards_word_t *w = muse_quiz_pool_word(i);
        assert(w != NULL);
        assert(strcmp(w->en, en[i]) == 0);
        assert(strcmp(w->zh, zh[i]) == 0);
        assert(w->art >= 0); /* every pool word has art */
    }
    assert(muse_quiz_pool_word(-1) == NULL);
    assert(muse_quiz_pool_word(16) == NULL);
}

static void check_deal(const muse_quiz_t *g)
{
    /* Five distinct answers. */
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        assert(g->questions[i].answer != NULL);
        for (int j = i + 1; j < MUSE_QUIZ_QUESTIONS; j++) {
            assert(g->questions[i].answer != g->questions[j].answer);
        }
    }
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        const muse_quiz_question_t *q = &g->questions[i];
        assert(q->correct >= 0 && q->correct < MUSE_QUIZ_OPTIONS);
        /* The three options are distinct, hold the answer exactly
         * once (in the correct slot), and neither distractor is the
         * answer. */
        int answer_count = 0;
        for (int k = 0; k < MUSE_QUIZ_OPTIONS; k++) {
            assert(q->options[k] != NULL);
            if (q->options[k] == q->answer) {
                answer_count++;
            }
            assert(k == q->correct
                       ? q->options[k] == q->answer
                       : q->options[k] != q->answer);
            for (int m = k + 1; m < MUSE_QUIZ_OPTIONS; m++) {
                assert(q->options[k] != q->options[m]);
            }
        }
        assert(answer_count == 1);
    }
}

static void test_deal(void)
{
    /* 200 rounds: the invariants must hold for every shuffle, not
     * one lucky draw. */
    srand(12345);
    for (int r = 0; r < 200; r++) {
        muse_quiz_t g;
        muse_quiz_init(&g);
        muse_quiz_start(&g);
        assert(g.phase == MUSE_QUIZ_PLAYING);
        assert(g.q == 0 && g.score == 0 && g.answered == 0);
        assert(g.elapsed_ms == 0 && g.feedback_ms == 0);
        check_deal(&g);
    }
    /* A restart re-deals (and bumps the generation). */
    muse_quiz_t g;
    muse_quiz_init(&g);
    muse_quiz_start(&g);
    uint32_t gen = g.quiz_gen;
    muse_quiz_restart(&g);
    assert(g.phase == MUSE_QUIZ_PLAYING);
    assert(g.quiz_gen == gen + 1);
    assert(g.q == 0 && g.score == 0 && g.answered == 0);
    check_deal(&g);
    /* Starting twice is a no-op (use restart after OVER). */
    muse_quiz_start(&g);
    assert(g.phase == MUSE_QUIZ_PLAYING);
}

static void tick_n(muse_quiz_t *g, int n)
{
    for (int i = 0; i < n; i++) {
        muse_quiz_tick(g, MUSE_QUIZ_TICK_MS);
    }
}

static void test_answer_feedback(void)
{
    srand(7);
    muse_quiz_t g;
    muse_quiz_init(&g);
    muse_quiz_start(&g);
    /* The first question's pronunciation is queued. */
    assert(muse_quiz_pending_audio(&g));
    muse_quiz_clear_pending_audio(&g);
    assert(!muse_quiz_pending_audio(&g));
    /* A wrong pick: +0, verdict 600 ms, taps ignored inside it. */
    int correct = g.questions[0].correct;
    int wrong = (correct + 1) % MUSE_QUIZ_OPTIONS;
    assert(!muse_quiz_answer(&g, wrong));
    assert(g.score == 0 && g.answered == 1);
    assert(!g.last_right && g.last_pick == wrong);
    assert(g.feedback_ms == MUSE_QUIZ_FEEDBACK_MS);
    assert(!muse_quiz_answer(&g, correct));
    assert(g.answered == 1); /* ignored during the verdict */
    tick_n(&g, MUSE_QUIZ_FEEDBACK_MS / MUSE_QUIZ_TICK_MS);
    assert(g.feedback_ms == 0 && g.q == 1);
    assert(g.last_pick == -1);
    assert(muse_quiz_pending_audio(&g)); /* next question reads itself */
    muse_quiz_clear_pending_audio(&g);
    /* A right pick: +1. */
    correct = g.questions[1].correct;
    assert(muse_quiz_answer(&g, correct));
    assert(g.score == 1 && g.answered == 2);
    assert(g.last_right && g.last_pick == correct);
    /* Out of range is refused. */
    tick_n(&g, MUSE_QUIZ_FEEDBACK_MS / MUSE_QUIZ_TICK_MS);
    assert(!muse_quiz_answer(&g, -1));
    assert(!muse_quiz_answer(&g, MUSE_QUIZ_OPTIONS));
    assert(g.answered == 2);
    /* Answer the rest right: Q1 was already right, so the last three
     * right picks bring the score to 4 (Q0 was wrong); OVER after
     * the last verdict. */
    for (int qi = 2; qi < MUSE_QUIZ_QUESTIONS; qi++) {
        assert(g.q == qi);
        assert(muse_quiz_answer(&g, g.questions[qi].correct));
        tick_n(&g, MUSE_QUIZ_FEEDBACK_MS / MUSE_QUIZ_TICK_MS);
    }
    assert(g.phase == MUSE_QUIZ_OVER);
    assert(g.score == 4 && g.answered == 5);
    /* No answers after OVER, no replay, no current answer. */
    assert(!muse_quiz_answer(&g, 0));
    assert(!muse_quiz_replay(&g));
    assert(muse_quiz_current_answer(&g) == NULL);
    /* A replay works while PLAYING and queues audio. */
    muse_quiz_t h;
    muse_quiz_init(&h);
    muse_quiz_start(&h);
    muse_quiz_clear_pending_audio(&h);
    assert(muse_quiz_replay(&h));
    assert(muse_quiz_pending_audio(&h));
    assert(muse_quiz_current_answer(&h) == h.questions[0].answer);
    /* Answers before start are refused. */
    muse_quiz_t fresh;
    muse_quiz_init(&fresh);
    assert(!muse_quiz_answer(&fresh, 0));
    assert(!muse_quiz_replay(&fresh));
    assert(muse_quiz_current_answer(&fresh) == NULL);
}

static void test_clock(void)
{
    srand(9);
    muse_quiz_t g;
    muse_quiz_init(&g);
    muse_quiz_start(&g);
    assert(muse_quiz_remaining_s(&g) == 60);
    muse_quiz_tick(&g, 1000);
    assert(muse_quiz_remaining_s(&g) == 59);
    muse_quiz_tick(&g, 500);
    assert(muse_quiz_remaining_s(&g) == 59); /* rounded up */
    /* 59.5 s in: 1 second left, still playing. */
    muse_quiz_tick(&g, 58000);
    assert(muse_quiz_remaining_s(&g) == 1);
    assert(g.phase == MUSE_QUIZ_PLAYING);
    muse_quiz_tick(&g, 500);
    assert(muse_quiz_remaining_s(&g) == 0);
    assert(g.phase == MUSE_QUIZ_OVER);
    /* The clock ends the round even mid-verdict. */
    muse_quiz_t h;
    muse_quiz_init(&h);
    muse_quiz_start(&h);
    assert(muse_quiz_answer(&h, h.questions[0].correct));
    assert(h.feedback_ms > 0);
    muse_quiz_tick(&h, MUSE_QUIZ_GAME_MS);
    assert(h.phase == MUSE_QUIZ_OVER);
    assert(h.feedback_ms == 0);
    /* Ticks outside PLAYING are no-ops. */
    uint32_t elapsed = h.elapsed_ms;
    muse_quiz_tick(&h, 1000);
    assert(h.elapsed_ms == elapsed);
}

static void test_cache(void)
{
    srand(11);
    muse_quiz_t g;
    muse_quiz_init(&g);
    muse_quiz_start(&g);
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        assert(g.cache[i] == MUSE_WORDCARDS_CACHE_EMPTY);
        assert(!muse_quiz_cache_is_ready(&g, i));
    }
    assert(!muse_quiz_cache_all_ready(&g));
    muse_quiz_cache_begin(&g);
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        assert(g.cache[i] == MUSE_WORDCARDS_CACHE_FETCHING);
    }
    muse_quiz_cache_ready(&g, 0, g.quiz_gen, 1234);
    assert(muse_quiz_cache_is_ready(&g, 0));
    assert(g.cache_bytes[0] == 1234);
    assert(!muse_quiz_cache_all_ready(&g));
    /* A second begin keeps a READY slot READY and re-arms the others. */
    muse_quiz_cache_begin(&g);
    assert(muse_quiz_cache_is_ready(&g, 0));
    assert(g.cache[1] == MUSE_WORDCARDS_CACHE_FETCHING);
    muse_quiz_cache_failed(&g, 1, g.quiz_gen);
    assert(g.cache[1] == MUSE_WORDCARDS_CACHE_FAILED);
    assert(g.cache_bytes[1] == 0);
    /* A mark from an older generation is ignored. */
    muse_quiz_restart(&g);
    assert(g.cache[0] == MUSE_WORDCARDS_CACHE_EMPTY);
    uint32_t old_gen = g.quiz_gen - 1;
    muse_quiz_cache_ready(&g, 2, old_gen, 999);
    assert(g.cache[2] != MUSE_WORDCARDS_CACHE_READY);
    muse_quiz_cache_failed(&g, 3, old_gen);
    assert(g.cache[3] != MUSE_WORDCARDS_CACHE_FAILED);
    /* The current generation's marks still land. */
    muse_quiz_cache_ready(&g, 2, g.quiz_gen, 2345);
    assert(muse_quiz_cache_is_ready(&g, 2));
    /* Out of range is ignored, never crashes. */
    muse_quiz_cache_ready(&g, -1, g.quiz_gen, 1);
    muse_quiz_cache_ready(&g, MUSE_QUIZ_QUESTIONS, g.quiz_gen, 1);
    muse_quiz_cache_failed(&g, -1, g.quiz_gen);
    assert(!muse_quiz_cache_is_ready(&g, -1));
    assert(!muse_quiz_cache_is_ready(&g, MUSE_QUIZ_QUESTIONS));
    /* All ready once every slot is marked. */
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        muse_quiz_cache_ready(&g, i, g.quiz_gen, 100 + (size_t)i);
    }
    assert(muse_quiz_cache_all_ready(&g));
}

int main(void)
{
    test_pool();
    test_deal();
    test_answer_feedback();
    test_clock();
    test_cache();
    printf("ok\n");
    return 0;
}

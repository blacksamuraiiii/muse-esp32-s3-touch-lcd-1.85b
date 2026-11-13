/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_quiz.c -- 英语口语（听力练习）logic, see muse_quiz.h.
 *
 * The round deals 5 questions up front (a partial Fisher-Yates over
 * the 20-word pool, so no answer repeats inside a round) and each
 * question's two distractors are drawn from the other 19 pool words,
 * so the answer never appears twice on one screen. The three options
 * are shuffled per question, so the right slot moves.
 *
 * The verdict window is the math game's rule: the clock keeps running
 * through it, answers are ignored inside it, and the next question is
 * dealt when it ends. The round ends at 60 seconds even mid-verdict.
 */
#include "muse_quiz.h"

#include <stdlib.h>

/* The pool: fruit (set 0, 4 words), animal (set 1, 8 words), food
 * (set 6, 4 words), things (set 8, 4 words) -- the word-cards sets
 * that are live. Food joined the pool 2026-11-13 (user decision,
 * review F5). A question's answer and its options are pointers into
 * this table, so there is no second copy of the words to keep in
 * step. */
static const struct {
    int set;
    int idx;
} k_pool[MUSE_QUIZ_POOL_WORDS] = {
    {0, 0}, {0, 1}, {0, 2}, {0, 3},
    {1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6}, {1, 7},
    {6, 0}, {6, 1}, {6, 2}, {6, 3},
    {8, 0}, {8, 1}, {8, 2}, {8, 3},
};

int muse_quiz_pool_words(void)
{
    return MUSE_QUIZ_POOL_WORDS;
}

const muse_wordcards_word_t *muse_quiz_pool_word(int i)
{
    if (i < 0 || i >= MUSE_QUIZ_POOL_WORDS) {
        return NULL;
    }
    return &MUSE_WORDCARDS_SETS_TABLE[k_pool[i].set].words[k_pool[i].idx];
}

void muse_quiz_init(muse_quiz_t *g)
{
    g->phase = MUSE_QUIZ_START;
    g->q = 0;
    g->score = 0;
    g->answered = 0;
    g->feedback_ms = 0;
    g->last_right = false;
    g->last_pick = -1;
    g->elapsed_ms = 0;
    g->audio_pending = false;
    g->quiz_gen = 0;
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        g->questions[i].answer = NULL;
        g->questions[i].correct = -1;
        for (int k = 0; k < MUSE_QUIZ_OPTIONS; k++) {
            g->questions[i].options[k] = NULL;
        }
        g->cache[i] = MUSE_WORDCARDS_CACHE_EMPTY;
        g->cache_bytes[i] = 0;
    }
}

/* Deal 5 questions: a partial Fisher-Yates over the pool picks 5
 * distinct answers; per question two distinct distractors are drawn
 * from the other 19 pool words and the three options are shuffled. */
static void deal(muse_quiz_t *g)
{
    int order[MUSE_QUIZ_POOL_WORDS];
    for (int i = 0; i < MUSE_QUIZ_POOL_WORDS; i++) {
        order[i] = i;
    }
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        int j = i + rand() % (MUSE_QUIZ_POOL_WORDS - i);
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int qi = 0; qi < MUSE_QUIZ_QUESTIONS; qi++) {
        muse_quiz_question_t *q = &g->questions[qi];
        const muse_wordcards_word_t *ans = muse_quiz_pool_word(order[qi]);
        q->answer = ans;
        /* Two distractors: distinct pool words, neither the answer.
         * Draw from the 19 non-answer words by index into a compacted
         * list (the answer's own pool slot is skipped). */
        int others[MUSE_QUIZ_POOL_WORDS - 1];
        int n = 0;
        for (int i = 0; i < MUSE_QUIZ_POOL_WORDS; i++) {
            if (i != order[qi]) {
                others[n++] = i;
            }
        }
        int d0 = rand() % n;
        int d1 = rand() % (n - 1);
        if (d1 >= d0) {
            d1++;
        }
        const muse_wordcards_word_t *dis[2] = {
            muse_quiz_pool_word(others[d0]),
            muse_quiz_pool_word(others[d1]),
        };
        /* Shuffle the three options: the answer lands in a random slot. */
        int slot = rand() % MUSE_QUIZ_OPTIONS;
        q->correct = slot;
        int di = 0;
        for (int k = 0; k < MUSE_QUIZ_OPTIONS; k++) {
            q->options[k] = (k == slot) ? ans : dis[di++];
        }
    }
}

static void reset_round(muse_quiz_t *g)
{
    g->q = 0;
    g->score = 0;
    g->answered = 0;
    g->feedback_ms = 0;
    g->last_right = false;
    g->last_pick = -1;
    g->elapsed_ms = 0;
    g->audio_pending = true; /* the first question reads itself */
    deal(g);
}

void muse_quiz_start(muse_quiz_t *g)
{
    if (g->phase != MUSE_QUIZ_START) {
        return;
    }
    reset_round(g);
    g->phase = MUSE_QUIZ_PLAYING;
}

void muse_quiz_restart(muse_quiz_t *g)
{
    /* A new round deals new words, so no cached byte survives it:
     * the marks are cleared and the generation bumped, exactly like
     * a word-cards set change (not a same-set restart, where the
     * words are identical and the cache stays valid). */
    g->quiz_gen++;
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        g->cache[i] = MUSE_WORDCARDS_CACHE_EMPTY;
        g->cache_bytes[i] = 0;
    }
    reset_round(g);
    g->phase = MUSE_QUIZ_PLAYING;
}

void muse_quiz_tick(muse_quiz_t *g, uint32_t ms)
{
    if (g->phase != MUSE_QUIZ_PLAYING) {
        return;
    }
    g->elapsed_ms += ms;
    if (g->feedback_ms > 0) {
        if (ms >= g->feedback_ms) {
            g->feedback_ms = 0;
        } else {
            g->feedback_ms -= ms;
        }
        if (g->feedback_ms == 0) {
            /* Verdict over: next question, or OVER after the last. */
            g->last_pick = -1;
            if (g->q + 1 >= MUSE_QUIZ_QUESTIONS) {
                g->phase = MUSE_QUIZ_OVER;
            } else {
                g->q++;
                g->audio_pending = true;
            }
        }
    }
    if (g->elapsed_ms >= MUSE_QUIZ_GAME_MS) {
        /* The clock ends the round even mid-verdict (the math game's
         * rule: the verdict is cleared with it). */
        g->feedback_ms = 0;
        g->phase = MUSE_QUIZ_OVER;
    }
}

bool muse_quiz_answer(muse_quiz_t *g, int opt)
{
    if (g->phase != MUSE_QUIZ_PLAYING || g->feedback_ms != 0) {
        return false;
    }
    if (opt < 0 || opt >= MUSE_QUIZ_OPTIONS) {
        return false;
    }
    g->answered++;
    g->last_pick = opt;
    g->last_right = (opt == g->questions[g->q].correct);
    if (g->last_right) {
        g->score++;
    }
    g->feedback_ms = MUSE_QUIZ_FEEDBACK_MS;
    return g->last_right;
}

bool muse_quiz_replay(muse_quiz_t *g)
{
    if (g->phase != MUSE_QUIZ_PLAYING) {
        return false;
    }
    g->audio_pending = true;
    return true;
}

bool muse_quiz_pending_audio(const muse_quiz_t *g)
{
    return g->audio_pending;
}

void muse_quiz_clear_pending_audio(muse_quiz_t *g)
{
    g->audio_pending = false;
}

const muse_wordcards_word_t *muse_quiz_current_answer(
    const muse_quiz_t *g)
{
    if (g->phase != MUSE_QUIZ_PLAYING) {
        return NULL;
    }
    return g->questions[g->q].answer;
}

uint32_t muse_quiz_remaining_s(const muse_quiz_t *g)
{
    if (g->elapsed_ms >= MUSE_QUIZ_GAME_MS) {
        return 0;
    }
    return (MUSE_QUIZ_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

void muse_quiz_cache_begin(muse_quiz_t *g)
{
    /* A slot already READY for this round stays READY: the UI calls
     * prefetch again every time the page is shown, and re-marking a
     * cached word FETCHING used to make the audio module re-download
     * it -- the word-cards cache_begin rule, same reason. */
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        if (g->cache[i] != MUSE_WORDCARDS_CACHE_READY) {
            g->cache[i] = MUSE_WORDCARDS_CACHE_FETCHING;
            g->cache_bytes[i] = 0;
        }
    }
}

void muse_quiz_cache_ready(muse_quiz_t *g, int i, uint32_t gen,
                           size_t bytes)
{
    if (i < 0 || i >= MUSE_QUIZ_QUESTIONS) {
        return;
    }
    if (gen != g->quiz_gen) {
        return; /* stale fetch from a previous round: ignore it */
    }
    g->cache[i] = MUSE_WORDCARDS_CACHE_READY;
    g->cache_bytes[i] = bytes;
}

void muse_quiz_cache_failed(muse_quiz_t *g, int i, uint32_t gen)
{
    if (i < 0 || i >= MUSE_QUIZ_QUESTIONS) {
        return;
    }
    if (gen != g->quiz_gen) {
        return; /* stale fetch from a previous round: ignore it */
    }
    g->cache[i] = MUSE_WORDCARDS_CACHE_FAILED;
    g->cache_bytes[i] = 0;
}

bool muse_quiz_cache_is_ready(const muse_quiz_t *g, int i)
{
    if (i < 0 || i >= MUSE_QUIZ_QUESTIONS) {
        return false;
    }
    return g->cache[i] == MUSE_WORDCARDS_CACHE_READY;
}

bool muse_quiz_cache_all_ready(const muse_quiz_t *g)
{
    for (int i = 0; i < MUSE_QUIZ_QUESTIONS; i++) {
        if (g->cache[i] != MUSE_WORDCARDS_CACHE_READY) {
            return false;
        }
    }
    return true;
}

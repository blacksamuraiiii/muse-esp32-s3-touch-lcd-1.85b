/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_quiz.h -- 英语口语（听力练习）logic (no LVGL, no ESP-IDF).
 *
 * One 60-second round of 5 questions (user decision 2026-11-11: was
 * 120 seconds): hear the English word, pick its picture from three
 * options. The question pool is the 20 words of the word-cards sets
 * that are already live -- fruit 4 + animal 8 + food 4 + things 4.
 * Each round deals 5 different answers (no repeat inside a round) and each
 * question's two distractors are two other pool words, never the
 * answer. The three options are shuffled, so the answer is not in a
 * fixed slot.
 *
 * A pick is graded at once: right is +1, wrong is +0 (never
 * negative). The verdict stays on screen for
 * MUSE_QUIZ_FEEDBACK_MS and during that window nothing is dealt and
 * no answer is taken -- a fast double tap can never skip a question.
 * The round clock keeps running through the verdict (it is round
 * time, the math game's rule).
 *
 * Score only ever goes up, so the settlement is 1 point per coin with
 * the shared 20-coin ceiling (a round tops out at 5 coins: 5 questions
 * x 1 point). Free entry (user decision 2026-11-11): no fee is charged
 * at start, so there is no fee-refused path and no charge in this
 * module -- the UI settles through muse_pet_round_coins_cap() at OVER,
 * the math game's shape.
 *
 * Timing is in milliseconds and advances only through
 * muse_quiz_tick(), so the host test drives it deterministically (no
 * lv_tick_get() here). The UI calls tick every 50 ms and pauses its
 * timer while the page is hidden, so tick() also no-ops unless
 * PLAYING. There is no PAUSED phase: leaving the page pauses the UI
 * timer (the clock stops with it) and coming back resumes it -- the
 * round never ends by itself while the player is away. Random picks
 * use stdlib rand(), seeded by the caller (the UI seeds once from
 * lv_tick_get on the first start, as the snake page does).
 *
 * Pronunciation reuses the word-cards pipeline (muse_wordcards_audio):
 * the round's 5 answer words are prefetched into PSRAM on entering
 * the page; a question's word plays from that cache at once and never
 * waits on the network. This module only keeps the bookkeeping for
 * that cache (one state per question slot, plus a generation so a
 * stale fetch cannot mark a word ready after the round changed);
 * the HTTP fetch and the minimp3 playback live in
 * muse_wordcards_audio.c, which is ESP-IDF-only. A word whose fetch
 * failed simply stays silent -- the question still shows, nothing
 * blocks, and no word is retried more than once per round.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_wordcards.h"

/* One round is 5 questions in 60 seconds (user 2026-11-11, was 120). */
#define MUSE_QUIZ_QUESTIONS 5
#define MUSE_QUIZ_OPTIONS 3
#define MUSE_QUIZ_POOL_WORDS 20
#define MUSE_QUIZ_GAME_MS 60000u
#define MUSE_QUIZ_TICK_MS 50u
/* How long the verdict stays up before the next question is dealt
 * (user 2026-11-08, the math game's number). Answer taps are ignored
 * during it. */
#define MUSE_QUIZ_FEEDBACK_MS 600u

typedef enum {
    MUSE_QUIZ_START = 0,
    MUSE_QUIZ_PLAYING,
    MUSE_QUIZ_OVER,
} muse_quiz_phase_t;

typedef struct {
    const muse_wordcards_word_t *answer;
    const muse_wordcards_word_t *options[MUSE_QUIZ_OPTIONS];
    int correct; /* option slot holding the answer, 0..2 */
} muse_quiz_question_t;

typedef struct {
    muse_quiz_phase_t phase;
    muse_quiz_question_t questions[MUSE_QUIZ_QUESTIONS];
    int q; /* current question, 0..MUSE_QUIZ_QUESTIONS-1 */
    uint32_t score;    /* +1 per right pick, never decreases */
    uint32_t answered; /* picks graded so far */
    uint32_t feedback_ms; /* > 0: verdict showing, no new question */
    bool last_right;   /* how the last pick was graded (for the UI) */
    int last_pick;     /* option the player picked, or -1 (for the UI) */
    uint32_t elapsed_ms;
    bool audio_pending; /* current answer word waiting to play */
    /* Prefetch cache bookkeeping, one slot per question (the slot's
     * word is that question's answer). Same states as the word
     * cards, same generation rule. */
    muse_wordcards_cache_t cache[MUSE_QUIZ_QUESTIONS];
    size_t cache_bytes[MUSE_QUIZ_QUESTIONS];
    uint32_t quiz_gen; /* bumped on every (re)start */
} muse_quiz_t;

/* The question pool: the live word-cards sets (fruit 4 + animal 8 +
 * food 4 + things 4), in table order. */
int muse_quiz_pool_words(void);
const muse_wordcards_word_t *muse_quiz_pool_word(int i);

void muse_quiz_init(muse_quiz_t *g);

/* START -> PLAYING, dealing 5 fresh questions and resetting the
 * round. No-op from any other phase (use restart for a new round
 * after OVER). The first question's pronunciation is queued as
 * pending audio, so the UI plays it from the cache as soon as it
 * is READY. */
void muse_quiz_start(muse_quiz_t *g);

/* Any phase -> PLAYING with a fresh round (new questions, new
 * generation, cache cleared: the new round's words differ, so no
 * cached byte survives it). */
void muse_quiz_restart(muse_quiz_t *g);

/* Advance the round by `ms` milliseconds. No-op unless PLAYING. Runs
 * the 60-second clock and the verdict window: when the verdict time
 * is up the next question is dealt (its pronunciation queued); when
 * the last question's verdict ends, or the clock runs out, the round
 * is OVER (which also clears a pending verdict). */
void muse_quiz_tick(muse_quiz_t *g, uint32_t ms);

/* Pick option `opt` (0..2) for the current question. Returns true
 * when the pick was right (score +1), false otherwise (score
 * unchanged). Either way the verdict shows for
 * MUSE_QUIZ_FEEDBACK_MS and the next question appears when it ends.
 * No-op (returns false, nothing changes) unless PLAYING, no verdict
 * is showing, and the option is in range. */
bool muse_quiz_answer(muse_quiz_t *g, int opt);

/* Queue the current question's pronunciation again (the speaker
 * button). False when not PLAYING. The UI drains the mark the same
 * way a first deal does. */
bool muse_quiz_replay(muse_quiz_t *g);

/* True when the current answer word is waiting to play. */
bool muse_quiz_pending_audio(const muse_quiz_t *g);

/* Clear a pending audio request (played, or given up on after a
 * failed fetch). */
void muse_quiz_clear_pending_audio(muse_quiz_t *g);

/* The current question's answer word (its English text is what the
 * speaker reads, its art is what the right option shows). NULL
 * outside PLAYING. */
const muse_wordcards_word_t *muse_quiz_current_answer(
    const muse_quiz_t *g);

/* Remaining whole seconds, rounded up (60 at start, 0 at the end). */
uint32_t muse_quiz_remaining_s(const muse_quiz_t *g);

/* --- prefetch cache bookkeeping (the fetch itself lives in
 * muse_wordcards_audio.c) --- */

/* Mark every question slot FETCHING at the start of a prefetch,
 * EXCEPT slots already READY for this generation: those keep their
 * bytes, so re-entering the page costs no network. Called once per
 * round entry, by the audio module. */
void muse_quiz_cache_begin(muse_quiz_t *g);

/* The fetch of question slot `i` finished: READY with `bytes` of
 * validated MP3, or FAILED. Stale marks (a fetch from an older
 * quiz generation) are ignored: the caller passes the generation
 * it fetched for. */
void muse_quiz_cache_ready(muse_quiz_t *g, int i, uint32_t gen,
                           size_t bytes);
void muse_quiz_cache_failed(muse_quiz_t *g, int i, uint32_t gen);

/* True when question slot `i`'s MP3 is in the cache and ready to play. */
bool muse_quiz_cache_is_ready(const muse_quiz_t *g, int i);

/* True when all five answer words are READY. */
bool muse_quiz_cache_all_ready(const muse_quiz_t *g);

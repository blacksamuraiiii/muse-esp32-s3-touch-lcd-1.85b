/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards_audio.h -- word-cards pronunciation fetch + playback.
 *
 * Pre-generated static MP3, NOT per-tap synthesis (user decision
 * 2026-11-10): the TTS bridge keeps one MP3 per word (see
 * ~/workspace/wordcards-tts-server.md) and answers the gadget's
 * request with the file straight from disk, so Azure is never called
 * for a flip. The request is the reply TTS's own /tts URL with the
 * word as its text (muse_tts_url_for_text): the bridge serves a
 * cached file when it has one, so the firmware needs no second
 * endpoint and no second host setting -- the word audio lives on the
 * same host the reply TTS already uses.
 *
 * Two tasks, both ESP-IDF-only (this file is not in the host tests):
 *
 *  - Prefetch: on entering a set, one background task fetches the
 *    set's 4 MP3s, one attempt per word, into PSRAM slots (32 KB cap
 *    per word, MP3 prefix gate). The request is the reply TTS's /tts
 *    URL with the word as its text plus word=1
 *    (muse_tts_url_for_text): the bridge answers a word=1 request
 *    with the pre-generated MP3 from its disk cache in the word voice
 *    (en-US-AriaNeural, -10%), never with the reply voice and never
 *    by synthesising. A word whose fetch fails is marked FAILED and
 *    stays silent; the card still flips and nothing blocks. A
 *    transport failure aborts the rest of the set (the host is
 *    unreachable -- three more 8 s timeouts would buy nothing). A
 *    fetch from an older set generation is ignored by the
 *    bookkeeping, so a slow fetch cannot resurrect audio for a round
 *    the player already left.
 *  - Playback: one task decodes a cached MP3 with minimp3 and writes
 *    it to the speaker through muse_audio_write (same shape as
 *    muse_audio_play.c's drain/resample, but from memory instead of
 *    HTTP). muse_audio_write already silences when the speaker is
 *    off and follows the volume slider.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "muse_quiz.h"
#include "muse_wordcards.h"

/* A list of words to fetch, with the caller's cache bookkeeping.
 * One fetch task serves every list, so the word cards and the quiz
 * share the TLS client, the fetch task and the play task -- but each
 * list has its OWN PSRAM slot pool (the word cards' 8 slots and the
 * quiz's 5), so one page's bytes can never overwrite the other's:
 * the two pages are never on screen together, yet a word-cards round
 * left mid-prefetch must not have its slots reused by the quiz.
 * Slots are 0..count-1 in list order; `en` points at static table
 * strings, so a list copied by value stays valid. The mark callbacks
 * are the caller's cache bookkeeping (word cards or quiz); they
 * ignore a stale generation themselves, exactly like the word-cards
 * marks always did. */
typedef struct {
    int count; /* words, 1..MUSE_WORDCARDS_WORDS */
    const char *en[MUSE_WORDCARDS_WORDS];
    uint8_t **slots; /* PSRAM slot pool, count entries of 32 KB */
    size_t *lens;    /* per-slot byte counts */
    void (*mark_begin)(void *ctx);
    void (*mark_ready)(void *ctx, int slot, uint32_t gen, size_t bytes);
    void (*mark_failed)(void *ctx, int slot, uint32_t gen);
    bool (*is_ready)(void *ctx, int slot);
    uint32_t (*cur_gen)(void *ctx);
    void *ctx;
} muse_wc_fetch_list_t;

/* Start prefetching the current set's 4 MP3s into PSRAM. Safe to
 * call repeatedly: only one prefetch runs per set generation, and a
 * second call for the same generation is a no-op. `g` is the UI's
 * static game struct and must stay alive while fetching. No-op when
 * the TTS template is empty (no speech service configured): every
 * word stays EMPTY and the cards simply stay silent. */
void muse_wordcards_audio_prefetch(muse_wordcards_t *g);

/* Start prefetching one word list into PSRAM (the word cards' set or
 * the quiz's round -- see muse_wc_fetch_list_t). Same queueing as
 * the set prefetch: a fetch already running is asked to stop and
 * this one starts when it exits. The list is copied by value. */
void muse_wordcards_audio_prefetch_list(const muse_wc_fetch_list_t *list);

/* Start prefetching the current round's 5 answer words into PSRAM
 * (the quiz's own slot pool). Same queueing as the word-cards
 * prefetch: a fetch already running is asked to stop and this one
 * starts when it exits. `g` is the UI's static quiz struct and must
 * stay alive while fetching. */
void muse_wordcards_audio_prefetch_quiz(muse_quiz_t *g);

/* Abort a prefetch in flight (leaving the tile / restarting). Marks
 * already fetched are kept; a fetch from the old generation is
 * ignored by the bookkeeping. */
void muse_wordcards_audio_prefetch_abort(void);

/* Play word `i`'s cached MP3 on its own task. Returns false when the
 * word is not READY, when a playback is already running, or when the
 * play task could not start. A replay while playing restarts it
 * (stop, then start again). */
bool muse_wordcards_audio_play(muse_wordcards_t *g, int i);

/* Play question slot `i`'s cached MP3 on its own task. Returns false
 * when the word is not READY, when a playback is already running, or
 * when the play task could not start. A replay while playing restarts
 * it (stop, then start again). Same queueing as the word-cards play:
 * a word asked for while another is playing is queued and starts
 * when the running one exits. */
bool muse_wordcards_audio_play_quiz(muse_quiz_t *g, int i);

/* Stop a playback in flight. Safe to call when idle. */
void muse_wordcards_audio_stop(void);

/* True while a playback task runs. */
bool muse_wordcards_audio_busy(void);

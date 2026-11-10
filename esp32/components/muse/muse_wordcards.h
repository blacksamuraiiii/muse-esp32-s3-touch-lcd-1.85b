/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards.h -- 英语单词卡片 logic (no LVGL, no ESP-IDF).
 *
 * Free learning mode only (user decision 2026-11-10): no entry fee,
 * no coins, no quiz mode -- the quiz (listen and pick the picture) is
 * a later phase and is deliberately NOT in this module. One round is
 * 2 minutes of PLAYING time; the clock stops while paused, exactly
 * like 毛毛跳跃's 60 s clock (user decision 2026-11-08, same rule).
 *
 * A round shows one set of 4 cards, 2x2, on the 240x240 field at
 * (60,60) -- cards are 116 px with an 8 px gap (mockup v4). The front
 * of a card is its picture only; the back is three centred lines:
 * the English word (gold, ~20 px), the Chinese (16 px Noto) and the
 * speaker logo (~31x18 px, v3 size). Tapping a front card flips it
 * to the back and plays its pronunciation; tapping the speaker logo
 * replays it; tapping anywhere else on a flipped card flips it back
 * to the front (user decision 2026-11-10).
 *
 * Pronunciation is pre-generated static MP3, NOT synthesised per tap
 * (user decision 2026-11-10): the TTS server keeps one MP3 per word
 * and answers the gadget's request with the file directly, so Azure
 * is never called for a flip. The UI prefetches the set's 4 MP3s
 * into PSRAM on entering the set; a flip plays from that cache at
 * once and never waits on the network. This module only keeps the
 * bookkeeping for that cache (one state per word, plus a generation
 * so a stale fetch cannot mark a word ready after the set changed);
 * the HTTP fetch and the minimp3 playback live in
 * muse_wordcards_audio.c, which is ESP-IDF-only. A word whose fetch
 * failed simply stays silent -- the card still flips, nothing
 * blocks, and no word is retried more than once per set entry.
 *
 * Word sets: 8 sets x 4 words were planned (fruit / animal / colour
 * / body / number / school / food / family, 32 words, per 人教新起点
 * 一年级上册 + 牛津初级). Phase 1 (user decision 2026-11-10) ships
 * the fruit and animal sets; the other six are data in the table
 * with their art marked MUSE_WORDCARDS_ART_NONE and a comment
 * naming the closest game-icons file, so adding a set is a data
 * change plus its four A8 masters -- no logic change. Do not ship
 * placeholder art that looks broken: a set without art is not offered.
 *
 * Timing is in milliseconds and advances only through
 * muse_wordcards_tick(), so the host test drives it deterministically
 * (no lv_tick_get() here). The UI calls tick every 50 ms and pauses
 * its timer while paused, so tick() also no-ops unless PLAYING.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One round lasts 2 minutes of PLAYING time (user 2026-11-10, free
 * learning mode). The clock runs only while PLAYING -- the UI pauses
 * its timer while paused, so tick() also no-ops unless PLAYING.
 * When it runs out the round ends on the summary page. */
#define MUSE_WORDCARDS_GAME_MS 120000u
#define MUSE_WORDCARDS_TICK_MS 50u
/* Field geometry, the same 240x240 square the other games use. */
#define MUSE_WORDCARDS_FIELD_X 60
#define MUSE_WORDCARDS_FIELD_Y 60
#define MUSE_WORDCARDS_FIELD_SIZE 240
#define MUSE_WORDCARDS_CARD 116
#define MUSE_WORDCARDS_GAP 8
#define MUSE_WORDCARDS_WORDS 4
#define MUSE_WORDCARDS_SETS 8
/* One pre-generated word MP3 is a few KB (a single word at 16 kHz
 * 32 kbps mono); 32 KB per word is far above any real one and keeps
 * a corrupt or wrong file from eating PSRAM. Four words fit in tens
 * of KB of the 8 MB PSRAM. */
#define MUSE_WORDCARDS_MAX_MP3_BYTES 32768u

/* No art master yet for this word (later sets). The UI must not offer
 * a set whose words have no art. */
#define MUSE_WORDCARDS_ART_NONE (-1)

typedef enum {
    MUSE_WORDCARDS_START = 0,
    MUSE_WORDCARDS_PLAYING,
    MUSE_WORDCARDS_PAUSED,
    MUSE_WORDCARDS_OVER,
} muse_wordcards_phase_t;

/* Prefetch state of one word's MP3 in the PSRAM cache. */
typedef enum {
    MUSE_WORDCARDS_CACHE_EMPTY = 0, /* nothing fetched for this set entry */
    MUSE_WORDCARDS_CACHE_FETCHING,  /* fetch in flight */
    MUSE_WORDCARDS_CACHE_READY,     /* bytes in PSRAM, MP3 prefix checked */
    MUSE_WORDCARDS_CACHE_FAILED,    /* fetch failed or payload was not MP3 */
} muse_wordcards_cache_t;

typedef struct {
    const char *en;   /* English word, lower-case [a-z]+ */
    const char *zh;   /* Chinese meaning, shown small under the word */
    uint32_t color;   /* recolour for the single-colour art master */
    int art;          /* index into the art table, or ART_NONE */
} muse_wordcards_word_t;

typedef struct {
    const char *title; /* e.g. "水果 · Fruit", the page title slot */
    muse_wordcards_word_t words[MUSE_WORDCARDS_WORDS];
} muse_wordcards_set_t;

typedef struct {
    muse_wordcards_phase_t phase;
    int set;                              /* index into the sets table */
    bool flipped[MUSE_WORDCARDS_WORDS];   /* true while the back shows */
    bool seen[MUSE_WORDCARDS_WORDS];      /* flipped at least once */
    uint32_t flipped_count;               /* distinct cards flipped */
    uint32_t elapsed_ms;                  /* PLAYING time this round */
    muse_wordcards_cache_t cache[MUSE_WORDCARDS_WORDS];
    size_t cache_bytes[MUSE_WORDCARDS_WORDS];
    uint32_t set_gen;                     /* bumped on every (re)start */
    int pending_audio;                    /* word waiting to play, or -1 */
} muse_wordcards_t;

/* The sets table (8 sets x 4 words). Sets 0 (fruit) and 1 (animal)
 * have art and are offered; the rest are data for later phases. */
extern const muse_wordcards_set_t MUSE_WORDCARDS_SETS_TABLE[MUSE_WORDCARDS_SETS];

/* How many sets are actually offered (fruit + animal in phase 1). */
int muse_wordcards_offered_sets(void);

/* True when every word of `set` has an art master. */
bool muse_wordcards_set_has_art(int set);

void muse_wordcards_init(muse_wordcards_t *g);

/* Switch to set `set` (0..MUSE_WORDCARDS_SETS-1) for a fresh round:
 * the round is reset to START with all cards front-up, the cache
 * marks are cleared to EMPTY, and set_gen is bumped so a prefetch
 * from the previous set cannot mark words ready in this one. The
 * caller then prefetches the new set's 4 MP3s. False (and no change)
 * when the set is out of range or has no art -- a set without art
 * is never offered, so this only fires on a programming error. */
bool muse_wordcards_select_set(muse_wordcards_t *g, int set);

/* START -> PLAYING, resetting the round and the cache bookkeeping
 * for the current set. No-op from any other phase (use restart for
 * a new round after OVER/PAUSED). */
void muse_wordcards_start(muse_wordcards_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_wordcards_pause(muse_wordcards_t *g);

/* PAUSED -> PLAYING. No-op from any other phase. */
void muse_wordcards_resume(muse_wordcards_t *g);

/* Any phase -> PLAYING with a fresh round (same set, reshuffled only
 * in the sense that every card is front-up again: the set order is
 * fixed, so "reshuffle" here means a clean repeat). Free, so no
 * charge is involved. Bumps set_gen, so a prefetch from the previous
 * round cannot mark words ready in this one. */
void muse_wordcards_restart(muse_wordcards_t *g);

/* Advance the round clock by `ms`. No-op unless PLAYING. Ends the
 * round (OVER) the moment the 2-minute clock runs out. */
void muse_wordcards_tick(muse_wordcards_t *g, uint32_t ms);

/* Whole seconds left in the round, rounded up (120 at the start, 0
 * at the end), for the HUD countdown. Same ceiling formula as
 * 金币雨 / 毛毛跳跃. */
uint32_t muse_wordcards_remaining_s(const muse_wordcards_t *g);

/* Flip card `i` (0..3) to its back. False when not PLAYING, when the
 * index is out of range, or when the card is already flipped. On
 * success the card counts as seen (the first flip of a card bumps
 * flipped_count) and its pronunciation is queued as pending_audio,
 * so the UI plays it from the cache as soon as it is READY. */
bool muse_wordcards_flip_to_back(muse_wordcards_t *g, int i);

/* Flip card `i` back to its front. False when not PLAYING, when the
 * index is out of range, or when the card is already front-up. A
 * flip back never un-sees the card: flipped_count keeps it. */
bool muse_wordcards_flip_to_front(muse_wordcards_t *g, int i);

/* Queue the pronunciation of card `i` again (the speaker logo).
 * False when not PLAYING, when the index is out of range, or when
 * the card is not flipped. Overwrites any pending audio: the latest
 * tap is the one that plays. */
bool muse_wordcards_replay(muse_wordcards_t *g, int i);

/* The word waiting for its audio, or -1 when nothing is queued. */
int muse_wordcards_pending_audio(const muse_wordcards_t *g);

/* Clear a pending audio request (played, or given up on after a
 * failed fetch). */
void muse_wordcards_clear_pending_audio(muse_wordcards_t *g);

/* --- prefetch cache bookkeeping (the fetch itself lives in
 * muse_wordcards_audio.c) --- */

/* Mark every word of the current set FETCHING at the start of a
 * prefetch. Called once per set entry, by the audio module. */
void muse_wordcards_cache_begin(muse_wordcards_t *g);

/* The fetch of word `i` finished: READY with `bytes` of validated
 * MP3, or FAILED. Stale marks (a fetch from an older set_gen) are
 * ignored: the caller passes the generation it fetched for. */
void muse_wordcards_cache_ready(muse_wordcards_t *g, int i, uint32_t gen,
                                size_t bytes);
void muse_wordcards_cache_failed(muse_wordcards_t *g, int i, uint32_t gen);

/* True when word `i`'s MP3 is in the cache and ready to play. */
bool muse_wordcards_cache_is_ready(const muse_wordcards_t *g, int i);

/* True when all four words are READY. */
bool muse_wordcards_cache_all_ready(const muse_wordcards_t *g);

/* Strict MP3 gate, the same rule as the bridge and the reply TTS:
 * longer than 64 bytes and starting with an MPEG frame sync (0xFF)
 * or an ID3 tag. An error page or a truncated body must never reach
 * the decoder. */
bool muse_wordcards_mp3_ok(const uint8_t *p, size_t n);

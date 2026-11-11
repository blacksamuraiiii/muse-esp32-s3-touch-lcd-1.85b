/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards.h -- 英语单词卡片 logic (no LVGL, no ESP-IDF).
 *
 * Free learning mode only (user decision 2026-11-10): no entry fee,
 * no coins, no quiz mode -- the quiz (listen and pick the picture) is
 * a later phase and is deliberately NOT in this module. A round has
 * NO time limit (user decision 2026-11-11, was 2 minutes): the cards
 * stay on screen until the player taps the back arrow, so there is no
 * clock, no countdown, no OVER phase and no "学了几张" summary. The
 * flipped count is still tracked (it is what the logic knows about
 * progress and the host tests pin it), it is simply never shown.
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
 * Word sets: 10 sets x 4 words (fruit / animal 1 / colour / body /
 * number / school / food / family / animal 2 / things, 40 words, per
 * 人教新起点一年级上册 + 牛津初级). Phase 1 (user decision
 * 2026-11-10) ships the fruit and animal 1 sets; phase 2 (user
 * decision 2026-11-11) adds animal 2 and things; the other six are
 * data in the table with their art marked MUSE_WORDCARDS_ART_NONE
 * and a comment naming the closest game-icons file, so adding a set
 * is a data change plus its four A8 masters -- no logic change. Do
 * not ship placeholder art that looks broken: a set without art is
 * not offered. Colours were explicitly rejected by the user
 * 2026-11-11 ("too simple") -- do not activate that set.
 *
 * Timing lives in the UI's own 50 ms timer, which only drains the
 * pending-audio mark and pokes the idle timer; the logic has no clock
 * at all, so the host test drives every state transition explicitly
 * (no lv_tick_get() here).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A round has no time limit (user 2026-11-11): the player leaves with
 * the back arrow, so there is no clock to tick down and no OVER state.
 * Field geometry, the same 240x240 square the other games use. */
#define MUSE_WORDCARDS_FIELD_X 60
#define MUSE_WORDCARDS_FIELD_Y 60
#define MUSE_WORDCARDS_FIELD_SIZE 240
#define MUSE_WORDCARDS_CARD 116
#define MUSE_WORDCARDS_GAP 8
/* A set holds up to 8 words (user decision 2026-11-11: animal 1 + animal 2
 * merged into one 8-word group), shown 4 per page on the card page; the
 * player swipes left/right between pages, the same horizontal strip idiom
 * the feed page uses. Sets with 4 words simply have one page. */
#define MUSE_WORDCARDS_WORDS 8
#define MUSE_WORDCARDS_PER_PAGE 4
#define MUSE_WORDCARDS_PAGES 2
#define MUSE_WORDCARDS_SETS 9
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
} muse_wordcards_phase_t;

/* Prefetch state of one word's MP3 in the PSRAM cache. */
typedef enum {
    MUSE_WORDCARDS_CACHE_EMPTY = 0, /* nothing fetched for this set entry */
    MUSE_WORDCARDS_CACHE_FETCHING,  /* fetch in flight */
    MUSE_WORDCARDS_CACHE_READY,     /* bytes in PSRAM, MP3 prefix checked */
    MUSE_WORDCARDS_CACHE_FAILED,    /* fetch failed or payload was not MP3 */
} muse_wordcards_cache_t;

typedef struct {
    const char *en;   /* English word, lower-case [a-z]+; NULL = unused slot */
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
    muse_wordcards_cache_t cache[MUSE_WORDCARDS_WORDS];
    size_t cache_bytes[MUSE_WORDCARDS_WORDS];
    uint32_t set_gen;                     /* bumped on every (re)start */
    int pending_audio;                    /* word waiting to play, or -1 */
} muse_wordcards_t;

/* The sets table (9 sets x up to 8 words). Set 0 (fruit), 1 (animal, 8
 * words), 6 (food, 1.5.3a B1) and 8 (things) have art and are offered;
 * the rest are data for later phases. Slots past a set's real word count
 * have en == NULL. */
extern const muse_wordcards_set_t MUSE_WORDCARDS_SETS_TABLE[MUSE_WORDCARDS_SETS];

/* How many sets are actually offered (fruit + animal + food + things). */
int muse_wordcards_offered_sets(void);

/* True when every word of `set` has an art master. */
bool muse_wordcards_set_has_art(int set);

/* How many words `set` really has (0 when it has no art): 4 for the
 * single-page sets, 8 for the merged animal set. */
int muse_wordcards_set_words(int set);

void muse_wordcards_init(muse_wordcards_t *g);

/* Switch to set `set` (0..MUSE_WORDCARDS_SETS-1) for a fresh round:
 * the round is reset to START with all cards front-up, the cache
 * marks are cleared to EMPTY, and set_gen is bumped so a prefetch
 * from the previous set cannot mark words ready in this one. The
 * caller then prefetches the new set's 4 MP3s. False (and no change)
 * when the set is out of range or has no art -- a set without art
 * is never offered, so this only fires on a programming error. */
bool muse_wordcards_select_set(muse_wordcards_t *g, int set);

/* START -> PLAYING, resetting the round for the current set. No-op
 * from any other phase (use restart for a clean repeat). */
void muse_wordcards_start(muse_wordcards_t *g);

/* PLAYING with a fresh round (same set, every card front-up again:
 * the set order is fixed, so "reshuffle" here means a clean repeat).
 * Free, so no charge is involved. Bumps set_gen, so a prefetch from
 * the previous round cannot mark words ready in this one. */
void muse_wordcards_restart(muse_wordcards_t *g);

/* No clock any more (user decision 2026-11-11): a round runs until the
 * player leaves the page, so the module has no tick() and no countdown
 * -- do not add one back without the user asking for a time limit. */

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
 * prefetch, EXCEPT words already READY for this generation: those keep
 * their bytes, so re-entering the page costs no network. Called once per
 * set entry, by the audio module. */
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

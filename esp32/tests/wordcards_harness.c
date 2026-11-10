/* Host harness for 英语单词卡片 (mockup v4, 2026-11-10).
 * Builds with plain cc on the host (same pattern as
 * tictactoe_harness.c):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I components/muse tests/wordcards_harness.c \
 *      components/muse/muse_wordcards.c -o /tmp/wordcards
 * Covers the pure logic only (muse_wordcards.c has no LVGL and no
 * ESP-IDF -- the UI owns the 50 ms timer, the flip animation and the
 * audio tasks, so the harness drives the clock by calling
 * muse_wordcards_tick() directly):
 *  1. Word table: 8 sets x 4 words, fruit set fully wired (apple /
 *     banana / cherry / peach, art 0..3, colours), every other set
 *     data-only (ART_NONE) and not offered.
 *  2. Flip state machine: flip to back counts a card once (distinct),
 *     flip back and forth never inflates the count, replay only works
 *     on a flipped card, flips are refused outside PLAYING.
 *  3. 2-minute clock: ticks only while PLAYING, pauses freeze it,
 *     OVER at exactly 120000 ms, remaining seconds count down.
 *  4. Cache bookkeeping: begin -> FETCHING, ready/failed marks,
 *     a mark from an older set generation is ignored (a slow fetch
 *     cannot resurrect audio for a round the player left), all-ready.
 *  5. MP3 prefix gate: the same >64 B + 0xFF/ID3 rule as the bridge.
 *  6. Pending audio: set by flip/replay, cleared on demand. (The UI
 *     also clears it when a card is flipped back or the round is
 *     paused -- that is UI behaviour, not logic, and is not covered
 *     here.)
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "muse_wordcards.h"

static void test_table(void)
{
    assert(MUSE_WORDCARDS_SETS == 8);
    assert(MUSE_WORDCARDS_WORDS == 4);
    /* Fruit set: fully wired, art 0..3, in table order. */
    const muse_wordcards_set_t *fruit = &MUSE_WORDCARDS_SETS_TABLE[0];
    assert(strcmp(fruit->title, "水果 · Fruit") == 0);
    assert(strcmp(fruit->words[0].en, "apple") == 0);
    assert(strcmp(fruit->words[0].zh, "苹果") == 0);
    assert(fruit->words[0].color == 0xff5c5c);
    assert(fruit->words[0].art == 0);
    assert(strcmp(fruit->words[1].en, "banana") == 0);
    assert(strcmp(fruit->words[1].zh, "香蕉") == 0);
    assert(fruit->words[1].color == 0xf6cb4b);
    assert(fruit->words[1].art == 1);
    assert(strcmp(fruit->words[2].en, "cherry") == 0);
    assert(strcmp(fruit->words[2].zh, "樱桃") == 0);
    assert(fruit->words[2].color == 0xff5c5c);
    assert(fruit->words[2].art == 2);
    assert(strcmp(fruit->words[3].en, "peach") == 0);
    assert(strcmp(fruit->words[3].zh, "桃子") == 0);
    assert(fruit->words[3].color == 0xffaa78);
    assert(fruit->words[3].art == 3);
    /* Every other set is data-only: no art, so not offered. */
    assert(muse_wordcards_offered_sets() == 1);
    assert(muse_wordcards_set_has_art(0));
    for (int s = 1; s < MUSE_WORDCARDS_SETS; s++) {
        assert(!muse_wordcards_set_has_art(s));
        for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
            assert(MUSE_WORDCARDS_SETS_TABLE[s].words[i].art ==
                   MUSE_WORDCARDS_ART_NONE);
            assert(MUSE_WORDCARDS_SETS_TABLE[s].words[i].en[0] != '\0');
            assert(MUSE_WORDCARDS_SETS_TABLE[s].words[i].zh[0] != '\0');
        }
    }
    assert(!muse_wordcards_set_has_art(-1));
    assert(!muse_wordcards_set_has_art(MUSE_WORDCARDS_SETS));
}

static muse_wordcards_t playing_game(void)
{
    muse_wordcards_t g;
    muse_wordcards_init(&g);
    muse_wordcards_start(&g);
    assert(g.phase == MUSE_WORDCARDS_PLAYING);
    return g;
}

static void test_flip_counts_distinct(void)
{
    muse_wordcards_t g = playing_game();
    assert(muse_wordcards_pending_audio(&g) == -1);
    /* A flip before start is refused (fresh init, START phase). */
    muse_wordcards_t fresh;
    muse_wordcards_init(&fresh);
    assert(!muse_wordcards_flip_to_back(&fresh, 0));
    assert(!muse_wordcards_flip_to_front(&fresh, 0));
    assert(!muse_wordcards_replay(&fresh, 0));
    /* First flip of card 0: counted, pending audio set. */
    assert(muse_wordcards_flip_to_back(&g, 0));
    assert(g.flipped[0] && g.seen[0]);
    assert(g.flipped_count == 1);
    assert(muse_wordcards_pending_audio(&g) == 0);
    /* Flipping the same card again is refused; replay works. */
    assert(!muse_wordcards_flip_to_back(&g, 0));
    assert(muse_wordcards_replay(&g, 0));
    assert(muse_wordcards_pending_audio(&g) == 0);
    /* Replay of a front card is refused. */
    assert(!muse_wordcards_replay(&g, 1));
    /* Flip back and forth: the count never inflates. */
    assert(muse_wordcards_flip_to_front(&g, 0));
    assert(!g.flipped[0] && g.seen[0]);
    assert(g.flipped_count == 1);
    assert(muse_wordcards_flip_to_back(&g, 0));
    assert(g.flipped_count == 1);
    /* Out of range is refused either way. */
    assert(!muse_wordcards_flip_to_back(&g, -1));
    assert(!muse_wordcards_flip_to_back(&g, MUSE_WORDCARDS_WORDS));
    assert(!muse_wordcards_flip_to_front(&g, -1));
    assert(!muse_wordcards_replay(&g, MUSE_WORDCARDS_WORDS));
    /* Flip the other three: the count reaches 4. */
    assert(muse_wordcards_flip_to_back(&g, 1));
    assert(muse_wordcards_flip_to_back(&g, 2));
    assert(muse_wordcards_flip_to_back(&g, 3));
    assert(g.flipped_count == 4);
    /* Clearing pending works. */
    muse_wordcards_clear_pending_audio(&g);
    assert(muse_wordcards_pending_audio(&g) == -1);
}

static void test_clock(void)
{
    muse_wordcards_t g = playing_game();
    assert(MUSE_WORDCARDS_GAME_MS == 120000u);
    assert(muse_wordcards_remaining_s(&g) == 120);
    muse_wordcards_tick(&g, 50);
    assert(g.elapsed_ms == 50);
    assert(muse_wordcards_remaining_s(&g) == 120);
    muse_wordcards_tick(&g, 950);
    assert(g.elapsed_ms == 1000);
    assert(muse_wordcards_remaining_s(&g) == 119);
    /* Pausing freezes the clock; the tick is a no-op. */
    muse_wordcards_pause(&g);
    assert(g.phase == MUSE_WORDCARDS_PAUSED);
    muse_wordcards_tick(&g, 60000);
    assert(g.elapsed_ms == 1000);
    assert(muse_wordcards_remaining_s(&g) == 119);
    /* Flips are refused while paused, and resume thaws the clock. */
    assert(!muse_wordcards_flip_to_back(&g, 0));
    muse_wordcards_resume(&g);
    assert(g.phase == MUSE_WORDCARDS_PLAYING);
    muse_wordcards_tick(&g, 1000);
    assert(g.elapsed_ms == 2000);
    /* The round ends at exactly 120000 ms, not one tick early. */
    muse_wordcards_tick(&g, 117999);
    assert(g.phase == MUSE_WORDCARDS_PLAYING);
    assert(muse_wordcards_remaining_s(&g) == 1);
    muse_wordcards_tick(&g, 1);
    assert(g.elapsed_ms == 120000);
    assert(g.phase == MUSE_WORDCARDS_OVER);
    assert(muse_wordcards_remaining_s(&g) == 0);
    /* Ticking past the end stays OVER; flips are refused. */
    muse_wordcards_tick(&g, 5000);
    assert(g.phase == MUSE_WORDCARDS_OVER);
    assert(g.elapsed_ms == 120000);
    assert(!muse_wordcards_flip_to_back(&g, 0));
    /* Restart: a fresh round, a new generation, same set. */
    uint32_t gen = g.set_gen;
    muse_wordcards_restart(&g);
    assert(g.phase == MUSE_WORDCARDS_PLAYING);
    assert(g.set_gen == gen + 1);
    assert(g.elapsed_ms == 0);
    assert(g.flipped_count == 0);
    assert(muse_wordcards_remaining_s(&g) == 120);
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        assert(!g.flipped[i] && !g.seen[i]);
    }
    /* A start from START does not bump the generation (the show-time
     * prefetch stays valid across the 开始 tap). */
    muse_wordcards_t h;
    muse_wordcards_init(&h);
    assert(h.set_gen == 0);
    muse_wordcards_start(&h);
    assert(h.set_gen == 0);
    /* Starting twice is a no-op. */
    muse_wordcards_start(&h);
    assert(h.phase == MUSE_WORDCARDS_PLAYING);
}

static void test_cache(void)
{
    muse_wordcards_t g = playing_game();
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        assert(g.cache[i] == MUSE_WORDCARDS_CACHE_EMPTY);
        assert(!muse_wordcards_cache_is_ready(&g, i));
    }
    assert(!muse_wordcards_cache_all_ready(&g));
    muse_wordcards_cache_begin(&g);
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        assert(g.cache[i] == MUSE_WORDCARDS_CACHE_FETCHING);
    }
    muse_wordcards_cache_ready(&g, 0, g.set_gen, 1234);
    assert(muse_wordcards_cache_is_ready(&g, 0));
    assert(g.cache_bytes[0] == 1234);
    assert(!muse_wordcards_cache_all_ready(&g));
    muse_wordcards_cache_failed(&g, 1, g.set_gen);
    assert(g.cache[1] == MUSE_WORDCARDS_CACHE_FAILED);
    assert(g.cache_bytes[1] == 0);
    /* A mark from an older generation is ignored: a slow fetch from
     * the previous round cannot resurrect audio for a card the player
     * already left behind. */
    muse_wordcards_restart(&g);
    uint32_t old_gen = g.set_gen - 1;
    muse_wordcards_cache_ready(&g, 2, old_gen, 999);
    assert(g.cache[2] != MUSE_WORDCARDS_CACHE_READY);
    muse_wordcards_cache_failed(&g, 3, old_gen);
    assert(g.cache[3] != MUSE_WORDCARDS_CACHE_FAILED);
    /* The current generation's marks still land. */
    muse_wordcards_cache_ready(&g, 2, g.set_gen, 2345);
    assert(muse_wordcards_cache_is_ready(&g, 2));
    /* Out of range is ignored, never crashes. */
    muse_wordcards_cache_ready(&g, -1, g.set_gen, 1);
    muse_wordcards_cache_ready(&g, MUSE_WORDCARDS_WORDS, g.set_gen, 1);
    muse_wordcards_cache_failed(&g, -1, g.set_gen);
    assert(!muse_wordcards_cache_is_ready(&g, -1));
    assert(!muse_wordcards_cache_is_ready(&g, MUSE_WORDCARDS_WORDS));
    /* All ready once every word is marked. */
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        muse_wordcards_cache_ready(&g, i, g.set_gen, 100 + (size_t)i);
    }
    assert(muse_wordcards_cache_all_ready(&g));
}

static void test_mp3_gate(void)
{
    uint8_t good_sync[65];
    memset(good_sync, 0, sizeof(good_sync));
    good_sync[0] = 0xFF;
    good_sync[1] = 0xFB;
    assert(muse_wordcards_mp3_ok(good_sync, sizeof(good_sync)));
    uint8_t good_id3[65];
    memset(good_id3, 0, sizeof(good_id3));
    memcpy(good_id3, "ID3", 3);
    assert(muse_wordcards_mp3_ok(good_id3, sizeof(good_id3)));
    /* Too short: the bridge's own rule is >64 bytes. */
    assert(!muse_wordcards_mp3_ok(good_sync, 64));
    assert(!muse_wordcards_mp3_ok(good_sync, 0));
    assert(!muse_wordcards_mp3_ok(NULL, 100));
    uint8_t bad[65];
    memset(bad, 'x', sizeof(bad));
    assert(!muse_wordcards_mp3_ok(bad, sizeof(bad)));
    uint8_t html[65];
    memset(html, 0, sizeof(html));
    memcpy(html, "<htm", 4);
    assert(!muse_wordcards_mp3_ok(html, sizeof(html)));
}

int main(void)
{
    test_table();
    test_flip_counts_distinct();
    test_clock();
    test_cache();
    test_mp3_gate();
    printf("ok\n");
    return 0;
}

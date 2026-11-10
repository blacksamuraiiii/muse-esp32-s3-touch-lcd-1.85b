/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards.c -- 英语单词卡片 logic, see muse_wordcards.h.
 *
 * The round is a 2-minute free learning session over one set of 4
 * cards. The clock runs only while PLAYING (the UI pauses its timer
 * while paused, and tick() no-ops otherwise), so a pause never eats
 * learning time -- the same rule as 毛毛跳跃's 60 s clock.
 *
 * flipped_count counts DISTINCT cards flipped at least once this
 * round: it is the "学了 X 张" number on the summary page, so
 * flipping one card back and forth, or replaying its pronunciation,
 * never inflates it. The per-word cache state is bookkeeping only:
 * the audio module owns the PSRAM bytes and moves a word from
 * FETCHING to READY/FAILED; a mark from an older set_gen is dropped,
 * so a slow fetch from a previous round cannot resurrect audio for
 * a card the player has already left behind.
 */
#include "muse_wordcards.h"

#include <string.h>

/* Colours are the theme values the v4 mockup used, per fruit. */
#define WC_RED 0xff5c5c
#define WC_YELLOW 0xf6cb4b
#define WC_PEACH 0xffaa78

const muse_wordcards_set_t MUSE_WORDCARDS_SETS_TABLE[MUSE_WORDCARDS_SETS] = {
    {
        "水果 · Fruit",
        {
            /* Art: game-icons, CC BY 3.0 (see muse_wordcards_art.h):
             * apple = lorc/shiny-apple, banana = delapouite/banana,
             * cherry = delapouite/cherry, peach = delapouite/peach. */
            {"apple", "苹果", WC_RED, 0},
            {"banana", "香蕉", WC_YELLOW, 1},
            {"cherry", "樱桃", WC_RED, 2},
            {"peach", "桃子", WC_PEACH, 3},
        },
    },
    {
        "动物 · Animal",
        {
            /* Art: game-icons, CC BY 3.0 (see muse_wordcards_art.h),
             * the closest matches, user-accepted 2026-11-10:
             * cat = lorc/cat, dog = delapouite/sitting-dog (no plain
             * dog in the set), bird = delapouite/hummingbird (no plain
             * bird in the set), fish = delapouite/tropical-fish.
             * Colours are the mockup's per-animal recolours. */
            {"cat", "猫", 0xe8a04c, 4},
            {"dog", "狗", 0xb07a4a, 5},
            {"bird", "鸟", 0x6ff0bf, 6},
            {"fish", "鱼", 0x6aa8ff, 7},
        },
    },
    {
        "颜色 · Colours",
        {
            /* No icon needed: the UI draws the colour itself as the
             * picture (a solid colour block). Art stays NONE until
             * that drawing exists. */
            {"red", "红色", 0xff5c5c, MUSE_WORDCARDS_ART_NONE},
            {"blue", "蓝色", 0x7dadcb, MUSE_WORDCARDS_ART_NONE},
            {"green", "绿色", 0x6ff0bf, MUSE_WORDCARDS_ART_NONE},
            {"yellow", "黄色", 0xf6cb4b, MUSE_WORDCARDS_ART_NONE},
        },
    },
    {
        "身体 · Body",
        {
            /* Art TODO (closest game-icons file, not yet generated):
             * eye = delapouite/eye-target, nose = delapouite/nose,
             * mouth = delapouite/lips, ear = delapouite/ear.
             * Verify the names in game-icons before generating. */
            {"eye", "眼睛", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"nose", "鼻子", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"mouth", "嘴", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"ear", "耳朵", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
        },
    },
    {
        "数字 · Numbers",
        {
            /* No icon needed: the UI draws the numeral itself.
             * Art stays NONE until that drawing exists. */
            {"one", "一", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"two", "二", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"three", "三", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"four", "四", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
        },
    },
    {
        "学校 · School",
        {
            /* Art TODO (closest game-icons file, not yet generated):
             * book = delapouite/book-cover, pen = delapouite/pencil,
             * bag = delapouite/school-bag, pencil = delapouite/pencil
             * (pen and pencil would share a master -- pick distinct
             * files before generating). */
            {"book", "书", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"pen", "笔", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"bag", "书包", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"pencil", "铅笔", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
        },
    },
    {
        "食物 · Food",
        {
            /* Art TODO (closest game-icons file, not yet generated):
             * cake = lorc/cake-slice (no whole cake in the set),
             * milk = rihlsul/milk-carton, bread = delapouite/bread,
             * egg = delapouite/egg-eye (verify the name in
             * game-icons before generating). */
            {"cake", "蛋糕", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"milk", "牛奶", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"bread", "面包", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"egg", "鸡蛋", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
        },
    },
    {
        "家庭 · Family",
        {
            /* No icon in game-icons fits a family member at 72 px
             * without looking like a game token: leave the art NONE
             * and draw the word's first letter, or find a dedicated
             * set, before offering this set. */
            {"mum", "妈妈", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"dad", "爸爸", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"sister", "姐妹", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
            {"brother", "兄弟", 0xf2efff, MUSE_WORDCARDS_ART_NONE},
        },
    },
};

int muse_wordcards_offered_sets(void)
{
    int n = 0;
    for (int s = 0; s < MUSE_WORDCARDS_SETS; s++) {
        if (muse_wordcards_set_has_art(s)) {
            n++;
        }
    }
    return n;
}

bool muse_wordcards_set_has_art(int set)
{
    if (set < 0 || set >= MUSE_WORDCARDS_SETS) {
        return false;
    }
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        if (MUSE_WORDCARDS_SETS_TABLE[set].words[i].art ==
            MUSE_WORDCARDS_ART_NONE) {
            return false;
        }
    }
    return true;
}

/* A round's play state. The cache marks are NOT touched here: they
 * belong to the set generation, not the round. A prefetch started when
 * the page was shown must survive the 开始 tap (start() resets the
 * round but keeps the generation), and only a restart -- which bumps
 * set_gen -- makes the fetch task re-mark them through
 * muse_wordcards_cache_begin(). */
static void reset_round(muse_wordcards_t *g)
{
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        g->flipped[i] = false;
        g->seen[i] = false;
    }
    g->flipped_count = 0;
    g->elapsed_ms = 0;
    g->pending_audio = -1;
}

void muse_wordcards_init(muse_wordcards_t *g)
{
    g->phase = MUSE_WORDCARDS_START;
    g->set = 0;
    g->set_gen = 0;
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        g->cache[i] = MUSE_WORDCARDS_CACHE_EMPTY;
        g->cache_bytes[i] = 0;
    }
    reset_round(g);
}

bool muse_wordcards_select_set(muse_wordcards_t *g, int set)
{
    if (set < 0 || set >= MUSE_WORDCARDS_SETS) {
        return false;
    }
    if (!muse_wordcards_set_has_art(set)) {
        return false;
    }
    g->set = set;
    /* A new set is a new generation: a prefetch from the previous
     * set must not mark words ready here, and the old cache marks
     * belong to the old words. The round restarts at START, so the
     * player sees the 开始 overlay for the new set. */
    g->set_gen++;
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        g->cache[i] = MUSE_WORDCARDS_CACHE_EMPTY;
        g->cache_bytes[i] = 0;
    }
    reset_round(g);
    g->phase = MUSE_WORDCARDS_START;
    return true;
}

void muse_wordcards_start(muse_wordcards_t *g)
{
    if (g->phase != MUSE_WORDCARDS_START) {
        return;
    }
    reset_round(g);
    g->phase = MUSE_WORDCARDS_PLAYING;
}

void muse_wordcards_pause(muse_wordcards_t *g)
{
    if (g->phase == MUSE_WORDCARDS_PLAYING) {
        g->phase = MUSE_WORDCARDS_PAUSED;
    }
}

void muse_wordcards_resume(muse_wordcards_t *g)
{
    if (g->phase == MUSE_WORDCARDS_PAUSED) {
        g->phase = MUSE_WORDCARDS_PLAYING;
    }
}

void muse_wordcards_restart(muse_wordcards_t *g)
{
    g->set_gen++;
    reset_round(g);
    g->phase = MUSE_WORDCARDS_PLAYING;
}

void muse_wordcards_tick(muse_wordcards_t *g, uint32_t ms)
{
    if (g->phase != MUSE_WORDCARDS_PLAYING) {
        return;
    }
    g->elapsed_ms += ms;
    if (g->elapsed_ms >= MUSE_WORDCARDS_GAME_MS) {
        g->elapsed_ms = MUSE_WORDCARDS_GAME_MS;
        g->phase = MUSE_WORDCARDS_OVER;
    }
}

uint32_t muse_wordcards_remaining_s(const muse_wordcards_t *g)
{
    if (g->elapsed_ms >= MUSE_WORDCARDS_GAME_MS) {
        return 0;
    }
    return (MUSE_WORDCARDS_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

bool muse_wordcards_flip_to_back(muse_wordcards_t *g, int i)
{
    if (g->phase != MUSE_WORDCARDS_PLAYING) {
        return false;
    }
    if (i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return false;
    }
    if (g->flipped[i]) {
        return false;
    }
    g->flipped[i] = true;
    if (!g->seen[i]) {
        g->seen[i] = true;
        g->flipped_count++;
    }
    g->pending_audio = i;
    return true;
}

bool muse_wordcards_flip_to_front(muse_wordcards_t *g, int i)
{
    if (g->phase != MUSE_WORDCARDS_PLAYING) {
        return false;
    }
    if (i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return false;
    }
    if (!g->flipped[i]) {
        return false;
    }
    g->flipped[i] = false;
    return true;
}

bool muse_wordcards_replay(muse_wordcards_t *g, int i)
{
    if (g->phase != MUSE_WORDCARDS_PLAYING) {
        return false;
    }
    if (i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return false;
    }
    if (!g->flipped[i]) {
        return false;
    }
    g->pending_audio = i;
    return true;
}

int muse_wordcards_pending_audio(const muse_wordcards_t *g)
{
    return g->pending_audio;
}

void muse_wordcards_clear_pending_audio(muse_wordcards_t *g)
{
    g->pending_audio = -1;
}

void muse_wordcards_cache_begin(muse_wordcards_t *g)
{
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        g->cache[i] = MUSE_WORDCARDS_CACHE_FETCHING;
        g->cache_bytes[i] = 0;
    }
}

void muse_wordcards_cache_ready(muse_wordcards_t *g, int i, uint32_t gen,
                                size_t bytes)
{
    if (i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return;
    }
    if (gen != g->set_gen) {
        return; /* stale fetch from a previous round: ignore it */
    }
    g->cache[i] = MUSE_WORDCARDS_CACHE_READY;
    g->cache_bytes[i] = bytes;
}

void muse_wordcards_cache_failed(muse_wordcards_t *g, int i, uint32_t gen)
{
    if (i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return;
    }
    if (gen != g->set_gen) {
        return; /* stale fetch from a previous round: ignore it */
    }
    g->cache[i] = MUSE_WORDCARDS_CACHE_FAILED;
    g->cache_bytes[i] = 0;
}

bool muse_wordcards_cache_is_ready(const muse_wordcards_t *g, int i)
{
    if (i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return false;
    }
    return g->cache[i] == MUSE_WORDCARDS_CACHE_READY;
}

bool muse_wordcards_cache_all_ready(const muse_wordcards_t *g)
{
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        if (g->cache[i] != MUSE_WORDCARDS_CACHE_READY) {
            return false;
        }
    }
    return true;
}

bool muse_wordcards_mp3_ok(const uint8_t *p, size_t n)
{
    if (!p || n <= 64) {
        return false;
    }
    if (p[0] == 0xFF) {
        return true; /* MPEG frame sync */
    }
    return n >= 3 && memcmp(p, "ID3", 3) == 0;
}

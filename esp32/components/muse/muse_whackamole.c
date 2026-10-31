/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_whackamole.c -- whack-a-mole logic, see muse_whackamole.h.
 *
 * No LVGL, no ESP-IDF: only stdlib rand() for hole picks, seeded by
 * the caller. Pacing follows the donor game (at most a couple of
 * moles up, each visible about a second) without copying its code:
 * the donor kept six near-duplicate per-hole timers and wrote past
 * a 2-slot timer array; here one tick drives every hole.
 */
#include "muse_whackamole.h"

#include <stdlib.h>

/* Spawn interval and mole visible time, in ms: 900 at first, 100 less
 * per 5 seconds played, floored at 500 (full speed from 20 seconds).
 * One curve serves both so a mole never outlives its spawn gap. */
static uint32_t pace_ms(uint32_t elapsed_ms)
{
    uint32_t pace = 900u - (elapsed_ms / 5000u) * 100u;
    return pace < 500u ? 500u : pace;
}

static int free_holes(const muse_whackamole_t *g, int *out)
{
    int n = 0;
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        if (g->holes[i].state == MUSE_WHACKAMOLE_EMPTY) {
            out[n++] = i;
        }
    }
    return n;
}

static void spawn_one(muse_whackamole_t *g)
{
    int free[MUSE_WHACKAMOLE_HOLES];
    int n = free_holes(g, free);
    if (n == 0) {
        return;
    }
    int hole = free[rand() % n];
    g->holes[hole].state = MUSE_WHACKAMOLE_UP;
    g->holes[hole].ms = 0;
    g->holes[hole].visible_ms = pace_ms(g->elapsed_ms);
}

static void reset_game(muse_whackamole_t *g)
{
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        g->holes[i].state = MUSE_WHACKAMOLE_EMPTY;
        g->holes[i].ms = 0;
        g->holes[i].visible_ms = 0;
    }
    g->score = 0;
    g->hits = 0;
    g->missed = 0;
    g->elapsed_ms = 0;
    g->spawn_ms = 0;
}

void muse_whackamole_init(muse_whackamole_t *g)
{
    g->phase = MUSE_WHACKAMOLE_START;
    reset_game(g);
}

void muse_whackamole_start(muse_whackamole_t *g)
{
    if (g->phase != MUSE_WHACKAMOLE_START) {
        return;
    }
    reset_game(g);
    g->phase = MUSE_WHACKAMOLE_PLAYING;
    /* One mole at once: the field is live from the first frame. */
    spawn_one(g);
}

void muse_whackamole_pause(muse_whackamole_t *g)
{
    if (g->phase == MUSE_WHACKAMOLE_PLAYING) {
        g->phase = MUSE_WHACKAMOLE_PAUSED;
    }
}

void muse_whackamole_resume(muse_whackamole_t *g)
{
    if (g->phase == MUSE_WHACKAMOLE_PAUSED) {
        g->phase = MUSE_WHACKAMOLE_PLAYING;
    }
}

void muse_whackamole_restart(muse_whackamole_t *g)
{
    reset_game(g);
    g->phase = MUSE_WHACKAMOLE_PLAYING;
    spawn_one(g);
}

void muse_whackamole_tick(muse_whackamole_t *g, uint32_t ms)
{
    if (g->phase != MUSE_WHACKAMOLE_PLAYING) {
        return;
    }
    g->elapsed_ms += ms;
    if (g->elapsed_ms >= MUSE_WHACKAMOLE_GAME_MS) {
        for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
            g->holes[i].state = MUSE_WHACKAMOLE_EMPTY;
            g->holes[i].ms = 0;
            g->holes[i].visible_ms = 0;
        }
        g->phase = MUSE_WHACKAMOLE_OVER;
        return;
    }
    /* Expire moles first so a freed slot can refill on this same tick. */
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        muse_whackamole_hole_t *hole = &g->holes[i];
        if (hole->state == MUSE_WHACKAMOLE_UP) {
            hole->ms += ms;
            if (hole->ms >= hole->visible_ms) {
                hole->state = MUSE_WHACKAMOLE_EMPTY;
                hole->ms = 0;
                hole->visible_ms = 0;
                g->missed++;
            }
        } else if (hole->state == MUSE_WHACKAMOLE_HIT) {
            hole->ms += ms;
            if (hole->ms >= MUSE_WHACKAMOLE_HIT_SHOW_MS) {
                hole->state = MUSE_WHACKAMOLE_EMPTY;
                hole->ms = 0;
                hole->visible_ms = 0;
            }
        }
    }
    /* Spawn burst: fill up to MAX_UP (at most 2 at once, the donor's
     * rhythm scaled to six holes). */
    g->spawn_ms += ms;
    if (g->spawn_ms >= pace_ms(g->elapsed_ms)) {
        g->spawn_ms = 0;
        while (muse_whackamole_up_count(g) < MUSE_WHACKAMOLE_MAX_UP) {
            int before = muse_whackamole_up_count(g);
            spawn_one(g);
            if (muse_whackamole_up_count(g) == before) {
                break; /* no free hole: cannot happen below MAX_UP */
            }
        }
    }
}

bool muse_whackamole_tap(muse_whackamole_t *g, int hole)
{
    if (g->phase != MUSE_WHACKAMOLE_PLAYING) {
        return false;
    }
    if (hole < 0 || hole >= MUSE_WHACKAMOLE_HOLES) {
        return false;
    }
    muse_whackamole_hole_t *h = &g->holes[hole];
    if (h->state != MUSE_WHACKAMOLE_UP) {
        return false;
    }
    h->state = MUSE_WHACKAMOLE_HIT;
    h->ms = 0;
    g->hits++;
    g->score++;
    return true;
}

int muse_whackamole_up_count(const muse_whackamole_t *g)
{
    int n = 0;
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        if (g->holes[i].state == MUSE_WHACKAMOLE_UP) {
            n++;
        }
    }
    return n;
}

uint32_t muse_whackamole_remaining_s(const muse_whackamole_t *g)
{
    if (g->elapsed_ms >= MUSE_WHACKAMOLE_GAME_MS) {
        return 0;
    }
    return (MUSE_WHACKAMOLE_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

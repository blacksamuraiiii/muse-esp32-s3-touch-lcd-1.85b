/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_goldrain.c -- 金币雨 logic, see muse_goldrain.h.
 *
 * Items fall straight down at the speed curve in the header. The
 * catcher stands on the field's bottom edge; a coin whose bottom edge
 * reaches the catch line while its centre is inside the sprite's span
 * is caught (1 point), a bomb caught the same way costs a life.
 * Anything that falls past the field's bottom edge is gone, coin or
 * bomb -- no penalty for letting a bomb drop.
 */
#include "muse_goldrain.h"

#include <stdlib.h>

static float clamp_catcher(float cx)
{
    float lo = (float)MUSE_GOLDRAIN_FIELD_X + (float)MUSE_GOLDRAIN_SPRITE_W / 2.0f;
    float hi = (float)(MUSE_GOLDRAIN_FIELD_X + MUSE_GOLDRAIN_FIELD_SIZE) -
               (float)MUSE_GOLDRAIN_SPRITE_W / 2.0f;
    if (cx < lo) {
        return lo;
    }
    if (cx > hi) {
        return hi;
    }
    return cx;
}

static void clear_items(muse_goldrain_t *g)
{
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        g->items[i].active = false;
    }
}

static void reset_game(muse_goldrain_t *g)
{
    clear_items(g);
    g->catcher_cx = (float)(MUSE_GOLDRAIN_FIELD_X + MUSE_GOLDRAIN_FIELD_SIZE / 2);
    g->score = 0;
    g->caught = 0;
    g->missed = 0;
    g->lives = MUSE_GOLDRAIN_LIVES;
    g->elapsed_ms = 0;
    g->spawn_ms = 0;
}

static void spawn_item(muse_goldrain_t *g)
{
    if (muse_goldrain_falling_count(g) >= MUSE_GOLDRAIN_MAX_FALLING) {
        return;
    }
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        if (g->items[i].active) {
            continue;
        }
        g->items[i].active = true;
        g->items[i].kind = (rand() % 100 < (int)MUSE_GOLDRAIN_BOMB_CHANCE_PCT)
                               ? MUSE_GOLDRAIN_BOMB
                               : MUSE_GOLDRAIN_COIN;
        /* Coin left edge: keep the whole 18 px coin inside the field,
         * with a 4 px margin so it never spawns half-clipped. */
        int span = MUSE_GOLDRAIN_FIELD_SIZE - 2 * 4 - MUSE_GOLDRAIN_COIN_PX;
        g->items[i].x = (float)(MUSE_GOLDRAIN_FIELD_X + 4 + (span > 0 ? rand() % span : 0));
        g->items[i].y = (float)(MUSE_GOLDRAIN_FIELD_Y - MUSE_GOLDRAIN_COIN_PX);
        g->spawn_ms = 0;
        return;
    }
}

void muse_goldrain_init(muse_goldrain_t *g)
{
    g->phase = MUSE_GOLDRAIN_START;
    reset_game(g);
}

void muse_goldrain_start(muse_goldrain_t *g)
{
    if (g->phase != MUSE_GOLDRAIN_START) {
        return;
    }
    reset_game(g);
    g->phase = MUSE_GOLDRAIN_PLAYING;
    /* One coin at once, so the field is never empty for the first
     * spawn interval (same as whack-a-mole). Force a coin, not a bomb:
     * the first thing a player sees should be catchable. */
    spawn_item(g);
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        if (g->items[i].active) {
            g->items[i].kind = MUSE_GOLDRAIN_COIN;
            break;
        }
    }
}

void muse_goldrain_pause(muse_goldrain_t *g)
{
    if (g->phase == MUSE_GOLDRAIN_PLAYING) {
        g->phase = MUSE_GOLDRAIN_PAUSED;
    }
}

void muse_goldrain_resume(muse_goldrain_t *g)
{
    if (g->phase == MUSE_GOLDRAIN_PAUSED) {
        g->phase = MUSE_GOLDRAIN_PLAYING;
    }
}

void muse_goldrain_restart(muse_goldrain_t *g)
{
    reset_game(g);
    g->phase = MUSE_GOLDRAIN_PLAYING;
    spawn_item(g);
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        if (g->items[i].active) {
            g->items[i].kind = MUSE_GOLDRAIN_COIN;
            break;
        }
    }
}

float muse_goldrain_speed_px_s(const muse_goldrain_t *g)
{
    float v = MUSE_GOLDRAIN_SPEED_START_PX_S +
              (float)(g->elapsed_ms / 1000) * MUSE_GOLDRAIN_SPEED_STEP_PX_S;
    if (v > MUSE_GOLDRAIN_SPEED_MAX_PX_S) {
        v = MUSE_GOLDRAIN_SPEED_MAX_PX_S;
    }
    return v;
}

uint32_t muse_goldrain_spawn_gap_ms(const muse_goldrain_t *g)
{
    uint32_t gap = MUSE_GOLDRAIN_SPAWN_START_MS -
                   (g->elapsed_ms / 1000) * MUSE_GOLDRAIN_SPAWN_STEP_MS;
    if (gap < MUSE_GOLDRAIN_SPAWN_MIN_MS) {
        gap = MUSE_GOLDRAIN_SPAWN_MIN_MS;
    }
    return gap;
}

int muse_goldrain_falling_count(const muse_goldrain_t *g)
{
    int n = 0;
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        if (g->items[i].active) {
            n++;
        }
    }
    return n;
}

uint32_t muse_goldrain_remaining_s(const muse_goldrain_t *g)
{
    if (g->elapsed_ms >= MUSE_GOLDRAIN_GAME_MS) {
        return 0;
    }
    return (MUSE_GOLDRAIN_GAME_MS - g->elapsed_ms + 999u) / 1000u;
}

void muse_goldrain_set_catcher(muse_goldrain_t *g, float cx)
{
    if (g->phase != MUSE_GOLDRAIN_PLAYING) {
        return;
    }
    g->catcher_cx = clamp_catcher(cx);
}

void muse_goldrain_tick(muse_goldrain_t *g, uint32_t ms)
{
    if (g->phase != MUSE_GOLDRAIN_PLAYING) {
        return;
    }
    g->elapsed_ms += ms;
    g->spawn_ms += ms;
    if (g->elapsed_ms >= MUSE_GOLDRAIN_GAME_MS) {
        g->phase = MUSE_GOLDRAIN_OVER;
        clear_items(g);
        return;
    }
    if (g->spawn_ms >= muse_goldrain_spawn_gap_ms(g)) {
        spawn_item(g);
    }
    float dy = muse_goldrain_speed_px_s(g) * (float)ms / 1000.0f;
    float left = g->catcher_cx - (float)MUSE_GOLDRAIN_SPRITE_W / 2.0f;
    float right = g->catcher_cx + (float)MUSE_GOLDRAIN_SPRITE_W / 2.0f;
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        muse_goldrain_item_t *it = &g->items[i];
        if (!it->active) {
            continue;
        }
        it->y += dy;
        float bottom = it->y + (float)MUSE_GOLDRAIN_COIN_PX;
        float centre = it->x + (float)MUSE_GOLDRAIN_COIN_PX / 2.0f;
        /* Caught only when the bottom edge CROSSES the catch line on
         * this tick while centred over the sprite: a coin that already
         * passed the hands (bottom below the line, centre outside) can
         * not be caught by sliding under it afterwards. */
        float prev_bottom = bottom - dy;
        if (prev_bottom < (float)MUSE_GOLDRAIN_CATCH_Y &&
            bottom >= (float)MUSE_GOLDRAIN_CATCH_Y &&
            centre >= left - 2.0f && centre <= right + 2.0f) {
            /* Caught: a coin scores, a bomb costs a life. */
            it->active = false;
            if (it->kind == MUSE_GOLDRAIN_COIN) {
                g->score++;
                g->caught++;
            } else {
                if (g->lives > 0) {
                    g->lives--;
                }
                if (g->lives == 0) {
                    g->phase = MUSE_GOLDRAIN_OVER;
                    clear_items(g);
                    return;
                }
            }
            continue;
        }
        if (bottom >= (float)(MUSE_GOLDRAIN_FIELD_Y + MUSE_GOLDRAIN_FIELD_SIZE)) {
            /* Fell past the bottom: gone, no penalty either way. */
            it->active = false;
            if (it->kind == MUSE_GOLDRAIN_COIN) {
                g->missed++;
            }
        }
    }
}

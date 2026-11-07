/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_doodle.c -- 毛毛飞行 logic, see muse_doodle.h.
 *
 * She flies: a tap sets vy to the flap speed and vx to the tap's
 * direction, gravity pulls her back down, and landing on a cloud
 * (falling onto its top) bounces her up again. Clouds are one-way:
 * she passes through them from below and only bounces when falling
 * onto them from above. The camera follows her up past CAM_Y and
 * clouds that scroll off the bottom are recycled above, so the climb
 * never ends; she falls out of the bottom when there is nothing left
 * to bounce on. Coins are the only score (+1 each, collected on
 * touch); clouds give no points.
 */
#include "muse_doodle.h"

#include <stdlib.h>

#define FIELD_LEFT ((float)MUSE_DOODLE_FIELD_X)
#define FIELD_RIGHT ((float)(MUSE_DOODLE_FIELD_X + MUSE_DOODLE_FIELD_SIZE))
#define FIELD_TOP ((float)MUSE_DOODLE_FIELD_Y)
#define FIELD_BOTTOM ((float)(MUSE_DOODLE_FIELD_Y + MUSE_DOODLE_FIELD_SIZE))

static void clear_all(muse_doodle_t *g)
{
    for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
        g->clouds[i].active = false;
    }
    for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
        g->coins[i].active = false;
    }
}

static muse_doodle_cloud_kind_t random_kind(void)
{
    /* Mostly static, some moving, a few breakable: the climb stays
     * fair at the start and the mix is the only difficulty knob. */
    int r = rand() % 100;
    if (r < 60) {
        return MUSE_DOODLE_CLOUD_STATIC;
    }
    if (r < 85) {
        return MUSE_DOODLE_CLOUD_MOVE;
    }
    return MUSE_DOODLE_CLOUD_BREAK;
}

static float random_cloud_x(void)
{
    int span = MUSE_DOODLE_FIELD_SIZE - MUSE_DOODLE_CLOUD_W;
    return FIELD_LEFT + (float)(span > 0 ? rand() % span : 0);
}

static void place_cloud(muse_doodle_t *g, int i, float x, float y,
                        muse_doodle_cloud_kind_t kind)
{
    g->clouds[i].active = true;
    g->clouds[i].kind = kind;
    g->clouds[i].x = x;
    g->clouds[i].y = y;
    g->clouds[i].vx = (kind == MUSE_DOODLE_CLOUD_MOVE)
                          ? ((rand() % 2 == 0) ? MUSE_DOODLE_MOVE_SPEED
                                               : -MUSE_DOODLE_MOVE_SPEED)
                          : 0.0f;
}

static void spawn_coin_above(muse_doodle_t *g, float cloud_x, float cloud_y)
{
    if (rand() % 100 >= (int)MUSE_DOODLE_COIN_CHANCE_PCT) {
        return;
    }
    for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
        if (g->coins[i].active) {
            continue;
        }
        g->coins[i].active = true;
        /* Just above the cloud, offset to the side so the trail
         * leads the flight path rather than sitting on the cloud. */
        float cx = cloud_x + 10.0f + (float)(rand() % 40);
        if (cx > FIELD_RIGHT - (float)MUSE_DOODLE_COIN_PX / 2.0f) {
            cx = FIELD_RIGHT - (float)MUSE_DOODLE_COIN_PX / 2.0f;
        }
        g->coins[i].x = cx;
        g->coins[i].y = cloud_y - 28.0f;
        return;
    }
}

static void reset_game(muse_doodle_t *g)
{
    clear_all(g);
    g->cx = (float)(MUSE_DOODLE_FIELD_X + MUSE_DOODLE_FIELD_SIZE / 2);
    g->cy = (float)(MUSE_DOODLE_FIELD_Y + MUSE_DOODLE_FIELD_SIZE) - 50.0f;
    g->vx = 0.0f;
    g->vy = 0.0f;
    g->score = 0;
    g->caught = 0;
    g->height_px = 0;
    /* Six cloud rows from the bottom up, ~70 px apart, so the field
     * starts full and the climb is already going. The two lowest are
     * static for a gentle start; the rest roll the usual mix. */
    float y = FIELD_BOTTOM - 15.0f;
    for (int i = 0; i < 6; i++) {
        muse_doodle_cloud_kind_t kind =
            (i < 2) ? MUSE_DOODLE_CLOUD_STATIC : random_kind();
        place_cloud(g, i, random_cloud_x(), y, kind);
        y -= (float)MUSE_DOODLE_CLOUD_SPACING;
    }
    /* Two coins on the opening trail, so the first thing she sees is
     * something to collect, not just clouds. */
    spawn_coin_above(g, g->clouds[1].x, g->clouds[1].y);
    spawn_coin_above(g, g->clouds[2].x, g->clouds[2].y);
}

void muse_doodle_init(muse_doodle_t *g)
{
    g->phase = MUSE_DOODLE_START;
    reset_game(g);
}

void muse_doodle_start(muse_doodle_t *g)
{
    if (g->phase != MUSE_DOODLE_START) {
        return;
    }
    reset_game(g);
    g->phase = MUSE_DOODLE_PLAYING;
}

void muse_doodle_pause(muse_doodle_t *g)
{
    if (g->phase == MUSE_DOODLE_PLAYING) {
        g->phase = MUSE_DOODLE_PAUSED;
    }
}

void muse_doodle_resume(muse_doodle_t *g)
{
    if (g->phase == MUSE_DOODLE_PAUSED) {
        g->phase = MUSE_DOODLE_PLAYING;
    }
}

void muse_doodle_restart(muse_doodle_t *g)
{
    reset_game(g);
    g->phase = MUSE_DOODLE_PLAYING;
}

void muse_doodle_flap(muse_doodle_t *g, int dir)
{
    if (g->phase != MUSE_DOODLE_PLAYING) {
        return;
    }
    g->vy = MUSE_DOODLE_FLAP_VY;
    g->vx = (dir < 0) ? -MUSE_DOODLE_H_SPEED : MUSE_DOODLE_H_SPEED;
}

int muse_doodle_cloud_count(const muse_doodle_t *g)
{
    int n = 0;
    for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
        if (g->clouds[i].active) {
            n++;
        }
    }
    return n;
}

int muse_doodle_coin_count(const muse_doodle_t *g)
{
    int n = 0;
    for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
        if (g->coins[i].active) {
            n++;
        }
    }
    return n;
}

/* The topmost (smallest y) active cloud, or FIELD_TOP when none is
 * active (never happens in play: reset always places six). */
static float topmost_cloud_y(const muse_doodle_t *g)
{
    float top = FIELD_TOP;
    bool any = false;
    for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
        if (!g->clouds[i].active) {
            continue;
        }
        if (!any || g->clouds[i].y < top) {
            top = g->clouds[i].y;
            any = true;
        }
    }
    (void)any;
    return top;
}

/* Put cloud `i` back at the top, above the highest cloud, with a
 * fresh roll of the dice -- and maybe a coin above it, so the trail
 * keeps leading upward. This keeps the cloud count constant: a cloud
 * that scrolls off the bottom or breaks under her is replaced at
 * once, so the field never thins out mid-climb. */
static void recycle_cloud_to_top(muse_doodle_t *g, int i)
{
    float nx = random_cloud_x();
    float ny = topmost_cloud_y(g) - (float)MUSE_DOODLE_CLOUD_SPACING;
    muse_doodle_cloud_kind_t kind = random_kind();
    place_cloud(g, i, nx, ny, kind);
    spawn_coin_above(g, nx, ny);
}

void muse_doodle_tick(muse_doodle_t *g, uint32_t ms)
{
    if (g->phase != MUSE_DOODLE_PLAYING) {
        return;
    }
    /* Physics is per 50 ms tick; a caller passing a different step
     * still advances by whole ticks so the tuning stays put. */
    uint32_t steps = ms / MUSE_DOODLE_TICK_MS;
    if (steps == 0) {
        steps = 1;
    }
    for (uint32_t s = 0; s < steps; s++) {
        /* Gravity, capped so a long fall stays catchable. */
        g->vy += MUSE_DOODLE_GRAVITY;
        if (g->vy > MUSE_DOODLE_MAX_FALL_VY) {
            g->vy = MUSE_DOODLE_MAX_FALL_VY;
        }
        float prev_feet = g->cy + (float)MUSE_DOODLE_SPRITE_H / 2.0f;
        g->cx += g->vx;
        g->cy += g->vy;
        /* Wrap around the field edges (classic Doodle Jump): out one
         * side, in the other. */
        if (g->cx < FIELD_LEFT) {
            g->cx += (float)MUSE_DOODLE_FIELD_SIZE;
        } else if (g->cx > FIELD_RIGHT) {
            g->cx -= (float)MUSE_DOODLE_FIELD_SIZE;
        }
        float feet = g->cy + (float)MUSE_DOODLE_SPRITE_H / 2.0f;
        /* Moving clouds slide and bounce off the field edges. */
        for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
            muse_doodle_cloud_t *c = &g->clouds[i];
            if (!c->active || c->kind != MUSE_DOODLE_CLOUD_MOVE) {
                continue;
            }
            c->x += c->vx;
            if (c->x < FIELD_LEFT) {
                c->x = FIELD_LEFT;
                c->vx = MUSE_DOODLE_MOVE_SPEED;
            } else if (c->x + (float)MUSE_DOODLE_CLOUD_W > FIELD_RIGHT) {
                c->x = FIELD_RIGHT - (float)MUSE_DOODLE_CLOUD_W;
                c->vx = -MUSE_DOODLE_MOVE_SPEED;
            }
        }
        /* Bounce: only while falling, only when her feet cross the
         * cloud's top on this tick, and only when her centre is over
         * the cloud (with a little forgiveness at the edges). Flying
         * up through a cloud never bounces -- clouds are one-way. */
        if (g->vy > 0.0f) {
            for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
                muse_doodle_cloud_t *c = &g->clouds[i];
                if (!c->active) {
                    continue;
                }
                if (prev_feet < c->y && feet >= c->y &&
                    g->cx >= c->x - 10.0f &&
                    g->cx <= c->x + (float)MUSE_DOODLE_CLOUD_W + 10.0f) {
                    g->vy = MUSE_DOODLE_BOUNCE_VY;
                    if (c->kind == MUSE_DOODLE_CLOUD_BREAK) {
                        /* One bounce and it is gone -- recycled to
                         * the top at once, so the field never thins. */
                        recycle_cloud_to_top(g, i);
                    }
                    break;
                }
            }
        }
        /* Camera: once she climbs past the line, the world scrolls
         * down by the excess and she stays on the line. */
        if (g->cy < (float)MUSE_DOODLE_CAM_Y) {
            float dy = (float)MUSE_DOODLE_CAM_Y - g->cy;
            g->cy = (float)MUSE_DOODLE_CAM_Y;
            g->height_px += (uint32_t)dy;
            for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
                if (g->clouds[i].active) {
                    g->clouds[i].y += dy;
                }
            }
            for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
                if (g->coins[i].active) {
                    g->coins[i].y += dy;
                }
            }
        }
        /* Recycle clouds that scrolled off the bottom: back to the
         * top, above the highest cloud, with a fresh roll of the
         * dice -- and maybe a coin above them, so the trail keeps
         * leading upward. */
        for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
            muse_doodle_cloud_t *c = &g->clouds[i];
            if (!c->active) {
                continue;
            }
            if (c->y <= FIELD_BOTTOM + 30.0f) {
                continue;
            }
            recycle_cloud_to_top(g, i);
        }
        /* Coins: collected on touch (a generous box, her sprite plus
         * half a coin on each side -- a kids' game should not demand
         * pixel-perfect flying). Coins that scroll off the bottom are
         * simply gone; no penalty, they were never hers. */
        for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
            muse_doodle_coin_t *coin = &g->coins[i];
            if (!coin->active) {
                continue;
            }
            float dx = g->cx - coin->x;
            float dy = g->cy - coin->y;
            if (dx < 0.0f) {
                dx = -dx;
            }
            if (dy < 0.0f) {
                dy = -dy;
            }
            if (dx < (float)MUSE_DOODLE_SPRITE_W / 2.0f +
                        (float)MUSE_DOODLE_COIN_PX / 2.0f &&
                dy < (float)MUSE_DOODLE_SPRITE_H / 2.0f +
                        (float)MUSE_DOODLE_COIN_PX / 2.0f) {
                coin->active = false;
                g->score++;
                g->caught++;
                continue;
            }
            if (coin->y > FIELD_BOTTOM + 30.0f) {
                coin->active = false;
            }
        }
        /* Fell out of the bottom: the round is over. The camera only
         * scrolls up, so once she is below the lowest cloud there is
         * nothing left to bounce on. */
        if (g->cy > FIELD_BOTTOM) {
            g->phase = MUSE_DOODLE_OVER;
            for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
                g->clouds[i].active = false;
            }
            for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
                g->coins[i].active = false;
            }
            return;
        }
    }
}

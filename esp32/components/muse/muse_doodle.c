/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_doodle.c -- 毛毛跳跃 logic, see muse_doodle.h.
 *
 * She hops. There is no upward input: a cloud is the only thing that
 * sends her up, and only when she is falling onto its top -- she passes
 * up through clouds, so a bounce is always the reward for landing.
 * Steering is horizontal (tilt plus a decaying shove from a tap). The
 * camera follows her up past CAM_Y and clouds that scroll off the
 * bottom are recycled above, so the climb never ends; she falls out of
 * the bottom when the player steers her off the clouds. Coins score
 * +1, clouds give no points, and there is no bomb (user 2026-11-07:
 * "取消炸弹惩罚"), so the score only ever goes up.
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

/* A new cloud is placed within reach of the cloud below it, not
 * anywhere across the field: a pure random x made a row above her
 * unlandable about half the time, so a round could stall on the bottom
 * cloud with no coin left to chase (measured on the host harness,
 * 2026-11-07). The window is ~1.5 cloud widths each way, which keeps
 * the climb varied but always steerable inside one bounce. */
#define MUSE_DOODLE_REACH_PX 100
static float random_cloud_x_near(float anchor_x)
{
    int span = MUSE_DOODLE_FIELD_SIZE - MUSE_DOODLE_CLOUD_W;
    float lo = anchor_x - (float)MUSE_DOODLE_REACH_PX;
    float hi = anchor_x + (float)MUSE_DOODLE_REACH_PX;
    if (lo < FIELD_LEFT) {
        lo = FIELD_LEFT;
    }
    if (hi > FIELD_LEFT + (float)span) {
        hi = FIELD_LEFT + (float)span;
    }
    int range = (int)(hi - lo);
    return lo + (float)(range > 0 ? rand() % (range + 1) : 0);
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

/* The coin above a cloud: just above it, offset to the side so the
 * trail leads the climb rather than sitting on the cloud. */
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
    g->score = 0;
    g->caught = 0;
    g->height_px = 0;
    g->tilt = 0.0f;
    g->shove = 0.0f;
    /* Six cloud rows from the bottom up, ~70 px apart, so the field
     * starts full and the climb is already going. The two lowest are
     * static for a gentle start; the rest roll the usual mix. */
    float y = FIELD_BOTTOM - 15.0f;
    float x = random_cloud_x_near((float)(MUSE_DOODLE_FIELD_X +
                                          MUSE_DOODLE_FIELD_SIZE / 2 -
                                          MUSE_DOODLE_CLOUD_W / 2));
    for (int i = 0; i < 6; i++) {
        muse_doodle_cloud_kind_t kind =
            (i < 2) ? MUSE_DOODLE_CLOUD_STATIC : random_kind();
        place_cloud(g, i, x, y, kind);
        y -= (float)MUSE_DOODLE_CLOUD_SPACING;
        x = random_cloud_x_near(x);
    }
    /* She starts STANDING ON the lowest cloud, feet just above its top,
     * so the first tick bounces her off it (user report 2026-11-07:
     * a restart kept her where she fell, mid-air, and she died again
     * before the player could react). */
    g->cx = g->clouds[0].x + (float)MUSE_DOODLE_CLOUD_W / 2.0f;
    g->cy = g->clouds[0].y - (float)MUSE_DOODLE_SPRITE_H / 2.0f - 1.0f;
    g->vx = 0.0f;
    g->vy = 0.0f;
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

void muse_doodle_tilt(muse_doodle_t *g, float tilt)
{
    if (g->phase != MUSE_DOODLE_PLAYING) {
        return;
    }
    if (tilt > 1.0f) {
        tilt = 1.0f;
    } else if (tilt < -1.0f) {
        tilt = -1.0f;
    }
    g->tilt = tilt;
}

void muse_doodle_tap(muse_doodle_t *g, int dir)
{
    if (g->phase != MUSE_DOODLE_PLAYING) {
        return;
    }
    g->shove = (dir < 0) ? -MUSE_DOODLE_TAP_VX : MUSE_DOODLE_TAP_VX;
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
 * fresh roll of the dice -- and maybe pickups above it, so the trail
 * keeps leading upward. This keeps the cloud count constant: a cloud
 * that scrolls off the bottom or breaks under her is replaced at
 * once, so the field never thins out mid-climb. */
static void recycle_cloud_to_top(muse_doodle_t *g, int i)
{
    /* Anchor the new top cloud on the one that is highest right now,
     * so the climb stays reachable however the recycle order falls. */
    float anchor = FIELD_LEFT;
    float top = 1e9f;
    for (int k = 0; k < MUSE_DOODLE_MAX_CLOUDS; k++) {
        if (g->clouds[k].active && g->clouds[k].y < top) {
            top = g->clouds[k].y;
            anchor = g->clouds[k].x;
        }
    }
    float nx = random_cloud_x_near(anchor);
    float ny = topmost_cloud_y(g) - (float)MUSE_DOODLE_CLOUD_SPACING;
    muse_doodle_cloud_kind_t kind = random_kind();
    place_cloud(g, i, nx, ny, kind);
    spawn_coin_above(g, nx, ny);
}

/* Would a pickup centred on (px, py) touch her? Generous: her sprite
 * plus half a pickup on each side -- a kids' game should not demand
 * pixel-perfect hovering. */
static bool touches(const muse_doodle_t *g, float px, float py, int size)
{
    float dx = g->cx - px;
    float dy = g->cy - py;
    if (dx < 0.0f) {
        dx = -dx;
    }
    if (dy < 0.0f) {
        dy = -dy;
    }
    return dx < (float)MUSE_DOODLE_SPRITE_W / 2.0f + (float)size / 2.0f &&
           dy < (float)MUSE_DOODLE_SPRITE_H / 2.0f + (float)size / 2.0f;
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
        /* Steering: the tilt holds a direction, a tap adds a shove that
         * fades, so the two controls sum instead of fighting. The shove
         * is spent first and decays after the move, so the tick right
         * after a tap actually runs at MUSE_DOODLE_TAP_VX. */
        g->vx = g->tilt * MUSE_DOODLE_H_SPEED + g->shove;
        if (g->vx > MUSE_DOODLE_MAX_VX) {
            g->vx = MUSE_DOODLE_MAX_VX;
        } else if (g->vx < -MUSE_DOODLE_MAX_VX) {
            g->vx = -MUSE_DOODLE_MAX_VX;
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
        /* The tap's shove fades one step per tick, after it has moved
         * her (see the steering block above). */
        g->shove *= MUSE_DOODLE_TAP_DECAY;
        if (g->shove < 0.05f && g->shove > -0.05f) {
            g->shove = 0.0f;
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
         * the cloud (with a little forgiveness at the edges). Rising
         * through a cloud never bounces -- clouds are one-way. */
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
         * dice -- and maybe a pickup above them, so the trail keeps
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
        /* Coins: +1 on touch. Coins that scroll off the bottom are
         * simply gone -- no penalty, they were never hers. */
        for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
            muse_doodle_coin_t *coin = &g->coins[i];
            if (!coin->active) {
                continue;
            }
            if (touches(g, coin->x, coin->y, MUSE_DOODLE_COIN_PX)) {
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

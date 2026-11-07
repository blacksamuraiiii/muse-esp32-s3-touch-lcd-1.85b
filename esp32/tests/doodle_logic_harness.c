/* 涂鸦跳跃 logic harness: checks muse_doodle.c without LVGL
 * or ESP-IDF. Run by tests/test_doodle_logic.py. Prints "ok" on
 * success; any failed check aborts via assert. Deterministic: the
 * random cloud/coin picks are seeded once with srand(), and the
 * bounce / coin / camera cases place things directly in the
 * (public) game struct instead of waiting for a random layout. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_doodle.h"

#define FIELD_BOTTOM (MUSE_DOODLE_FIELD_Y + MUSE_DOODLE_FIELD_SIZE)

static void tick_n(muse_doodle_t *g, int n)
{
    for (int i = 0; i < n; i++) {
        muse_doodle_tick(g, MUSE_DOODLE_TICK_MS);
    }
}

static void clear_clouds(muse_doodle_t *g)
{
    for (int i = 0; i < MUSE_DOODLE_MAX_CLOUDS; i++) {
        g->clouds[i].active = false;
    }
}

static void clear_coins(muse_doodle_t *g)
{
    for (int i = 0; i < MUSE_DOODLE_MAX_COINS; i++) {
        g->coins[i].active = false;
    }
}

/* One static cloud in slot 0, centred at cx, top at y. */
static void place_cloud(muse_doodle_t *g, float cx, float y)
{
    g->clouds[0].active = true;
    g->clouds[0].kind = MUSE_DOODLE_CLOUD_STATIC;
    g->clouds[0].x = cx - (float)MUSE_DOODLE_CLOUD_W / 2.0f;
    g->clouds[0].y = y;
    g->clouds[0].vx = 0.0f;
}

/* One coin in slot 0 at its centre. */
static void place_coin(muse_doodle_t *g, float cx, float cy)
{
    g->coins[0].active = true;
    g->coins[0].x = cx;
    g->coins[0].y = cy;
}

int main(void)
{
    srand(12345);

    /* Init state: six clouds, two lowest static, a couple of coins
     * on the opening trail. */
    muse_doodle_t g;
    muse_doodle_init(&g);
    assert(g.phase == MUSE_DOODLE_START);
    assert(g.score == 0 && g.caught == 0 && g.height_px == 0);
    assert(muse_doodle_cloud_count(&g) == 6);
    assert(g.clouds[0].kind == MUSE_DOODLE_CLOUD_STATIC);
    assert(g.clouds[1].kind == MUSE_DOODLE_CLOUD_STATIC);
    assert(muse_doodle_coin_count(&g) <= 2);
    assert(fabsf(g.cx - 180.0f) < 0.01f);

    /* Phase guards before start. */
    muse_doodle_pause(&g);
    assert(g.phase == MUSE_DOODLE_START);
    muse_doodle_resume(&g);
    assert(g.phase == MUSE_DOODLE_START);
    muse_doodle_tick(&g, 50);
    assert(fabsf(g.cy - 250.0f) < 0.01f); /* untouched */
    muse_doodle_flap(&g, -1);
    assert(fabsf(g.vy - 0.0f) < 0.01f && fabsf(g.vx - 0.0f) < 0.01f);

    /* Start: playing, same clouds, no score yet. */
    muse_doodle_start(&g);
    assert(g.phase == MUSE_DOODLE_PLAYING);
    assert(muse_doodle_cloud_count(&g) == 6);
    muse_doodle_start(&g); /* no-op from PLAYING */
    assert(g.phase == MUSE_DOODLE_PLAYING);

    /* Flap: vy is the flap speed, vx follows the tap's half. */
    muse_doodle_flap(&g, -1);
    assert(fabsf(g.vy - MUSE_DOODLE_FLAP_VY) < 0.01f);
    assert(fabsf(g.vx - (-MUSE_DOODLE_H_SPEED)) < 0.01f);
    muse_doodle_flap(&g, 1);
    assert(fabsf(g.vx - MUSE_DOODLE_H_SPEED) < 0.01f);

    /* Gravity: one tick without a flap, vy grows by 0.4 and she
     * falls by that much (vx keeps her drifting right). */
    g.vy = 0.0f;
    g.vx = 0.0f;
    float cy0 = g.cy;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(fabsf(g.vy - 0.4f) < 0.01f);
    assert(fabsf(g.cy - (cy0 + 0.4f)) < 0.01f);

    /* Wrap-around: out the left edge, in the right. */
    clear_clouds(&g);
    clear_coins(&g);
    g.cx = 61.0f;
    g.cy = 200.0f;
    g.vx = -MUSE_DOODLE_H_SPEED;
    g.vy = 0.0f;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(fabsf(g.cx - 298.0f) < 0.01f);

    /* Bounce: falling onto a cloud's top bounces her back up, and
     * the cloud stays (static). */
    clear_clouds(&g);
    clear_coins(&g);
    place_cloud(&g, 180.0f, 215.0f);
    g.cx = 180.0f;
    g.cy = 215.0f - (float)MUSE_DOODLE_SPRITE_H / 2.0f - 1.0f;
    g.vx = 0.0f;
    g.vy = 2.0f;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(fabsf(g.vy - MUSE_DOODLE_BOUNCE_VY) < 0.01f);
    assert(g.clouds[0].active);

    /* One-way: flying up through a cloud never bounces. */
    clear_clouds(&g);
    clear_coins(&g);
    place_cloud(&g, 180.0f, 145.0f);
    g.cx = 180.0f;
    g.cy = 145.0f + (float)MUSE_DOODLE_SPRITE_H / 2.0f + 5.0f;
    g.vx = 0.0f;
    g.vy = -5.0f;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(g.vy < 0.0f); /* still flying up, no bounce */

    /* A breakable cloud breaks after one bounce -- and is recycled
     * to the top at once, so the cloud count stays constant. */
    clear_clouds(&g);
    clear_coins(&g);
    place_cloud(&g, 180.0f, 215.0f);
    g.clouds[0].kind = MUSE_DOODLE_CLOUD_BREAK;
    g.cx = 180.0f;
    g.cy = 215.0f - (float)MUSE_DOODLE_SPRITE_H / 2.0f - 1.0f;
    g.vx = 0.0f;
    g.vy = 2.0f;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(fabsf(g.vy - MUSE_DOODLE_BOUNCE_VY) < 0.01f);
    assert(muse_doodle_cloud_count(&g) == 1);
    assert(g.clouds[0].y < 215.0f); /* recycled above */

    /* Coin: touching one scores +1 and it is gone. Clouds give no
     * points -- the score is the coins and nothing else. */
    clear_clouds(&g);
    clear_coins(&g);
    place_cloud(&g, 180.0f, 285.0f);
    place_coin(&g, 180.0f, 200.0f);
    g.cx = 180.0f;
    g.cy = 200.0f;
    g.vx = 0.0f;
    g.vy = 0.0f;
    uint32_t score0 = g.score;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(g.score == score0 + 1 && g.caught == 1);
    assert(!g.coins[0].active);
    /* Bouncing on the cloud below changed nothing about the score. */
    g.cy = 285.0f - (float)MUSE_DOODLE_SPRITE_H / 2.0f - 1.0f;
    g.vy = 2.0f;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(fabsf(g.vy - MUSE_DOODLE_BOUNCE_VY) < 0.01f);
    assert(g.score == score0 + 1);

    /* Camera: climbing past the line scrolls the world down and she
     * stays on the line; the height climbed is tracked. */
    clear_clouds(&g);
    clear_coins(&g);
    place_cloud(&g, 180.0f, 285.0f);
    float cloud_y0 = g.clouds[0].y;
    g.cx = 180.0f;
    g.cy = 120.0f;
    g.vx = 0.0f;
    g.vy = -5.0f;
    muse_doodle_tick(&g, MUSE_DOODLE_TICK_MS);
    assert(fabsf(g.cy - (float)MUSE_DOODLE_CAM_Y) < 0.01f);
    assert(g.height_px > 0);
    assert(g.clouds[0].y > cloud_y0); /* the world scrolled down */

    /* Pause freezes her; resume continues. */
    float cy_frozen = g.cy;
    muse_doodle_pause(&g);
    assert(g.phase == MUSE_DOODLE_PAUSED);
    tick_n(&g, 10);
    assert(fabsf(g.cy - cy_frozen) < 0.01f);
    muse_doodle_resume(&g);
    assert(g.phase == MUSE_DOODLE_PLAYING);

    /* Game over: falling out of the bottom with nothing to bounce
     * on ends the round. */
    clear_clouds(&g);
    clear_coins(&g);
    g.cx = 180.0f;
    g.cy = 280.0f;
    g.vx = 0.0f;
    g.vy = 15.0f;
    tick_n(&g, 2);
    assert(g.phase == MUSE_DOODLE_OVER);
    assert(muse_doodle_cloud_count(&g) == 0);
    assert(muse_doodle_coin_count(&g) == 0);

    /* Restart after OVER: fresh game, fee is the UI's job. */
    muse_doodle_restart(&g);
    assert(g.phase == MUSE_DOODLE_PLAYING);
    assert(g.score == 0);
    assert(muse_doodle_cloud_count(&g) == 6);

    printf("ok\n");
    return 0;
}

/* 金币雨 logic harness: checks muse_goldrain.c without LVGL
 * or ESP-IDF. Run by tests/test_goldrain_logic.py. Prints "ok" on
 * success; any failed check aborts via assert. Deterministic: the
 * random spawn picks are seeded once with srand(), and the bomb
 * cases place items directly in the (public) game struct instead
 * of waiting for a random bomb spawn. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_goldrain.h"

#define CATCH_LINE MUSE_GOLDRAIN_CATCH_Y
#define FIELD_BOTTOM (MUSE_GOLDRAIN_FIELD_Y + MUSE_GOLDRAIN_FIELD_SIZE)

static void tick_n(muse_goldrain_t *g, int n)
{
    for (int i = 0; i < n; i++) {
        muse_goldrain_tick(g, MUSE_GOLDRAIN_TICK_MS);
    }
}

/* Put one item in slot 0 at the given centre x / top y. */
static void place_item(muse_goldrain_t *g, muse_goldrain_kind_t kind,
                       float centre_x, float y)
{
    g->items[0].active = true;
    g->items[0].kind = kind;
    g->items[0].x = centre_x - (float)MUSE_GOLDRAIN_COIN_PX / 2.0f;
    g->items[0].y = y;
}

static void clear_all(muse_goldrain_t *g)
{
    for (int i = 0; i < MUSE_GOLDRAIN_MAX_ITEMS; i++) {
        g->items[i].active = false;
    }
    g->spawn_ms = 0;
}

int main(void)
{
    srand(12345);

    /* Init state. */
    muse_goldrain_t g;
    muse_goldrain_init(&g);
    assert(g.phase == MUSE_GOLDRAIN_START);
    assert(g.score == 0 && g.caught == 0 && g.missed == 0);
    assert(g.lives == MUSE_GOLDRAIN_LIVES);
    assert(g.elapsed_ms == 0);
    assert(muse_goldrain_falling_count(&g) == 0);
    assert(muse_goldrain_remaining_s(&g) == 30);
    assert(fabsf(muse_goldrain_speed_px_s(&g) - 90.0f) < 0.01f);
    assert(muse_goldrain_spawn_gap_ms(&g) == 950);

    /* Phase guards before start. */
    muse_goldrain_pause(&g);
    assert(g.phase == MUSE_GOLDRAIN_START);
    muse_goldrain_resume(&g);
    assert(g.phase == MUSE_GOLDRAIN_START);
    muse_goldrain_tick(&g, 50);
    assert(g.elapsed_ms == 0);
    muse_goldrain_set_catcher(&g, 100.0f);
    assert(fabsf(g.catcher_cx - 180.0f) < 0.01f); /* centre, untouched */

    /* Start: one coin at once, never a bomb. */
    muse_goldrain_start(&g);
    assert(g.phase == MUSE_GOLDRAIN_PLAYING);
    assert(muse_goldrain_falling_count(&g) == 1);
    assert(g.items[0].kind == MUSE_GOLDRAIN_COIN);
    assert(fabsf(g.catcher_cx - 180.0f) < 0.01f);
    muse_goldrain_start(&g); /* no-op from PLAYING */
    assert(muse_goldrain_falling_count(&g) == 1);

    /* Fall speed: 90 px/s at the start, 4.5 px per 50 ms tick. */
    float y0 = g.items[0].y;
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS);
    assert(fabsf(g.items[0].y - (y0 + 4.5f)) < 0.01f);

    /* Spawn gap: no second item before 950 ms. */
    tick_n(&g, 17); /* 18 ticks total = 900 ms */
    assert(muse_goldrain_falling_count(&g) == 1);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS); /* 950 ms */
    assert(muse_goldrain_falling_count(&g) == 2);

    /* Catch: a coin crossing the catch line over the catcher scores. */
    clear_all(&g);
    muse_goldrain_set_catcher(&g, 180.0f);
    place_item(&g, MUSE_GOLDRAIN_COIN, 180.0f,
               (float)CATCH_LINE - (float)MUSE_GOLDRAIN_COIN_PX - 2.0f);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS);
    assert(g.score == 1 && g.caught == 1);
    assert(muse_goldrain_falling_count(&g) == 0);

    /* A coin that already passed the hands is not caught by sliding
     * under it afterwards. */
    clear_all(&g);
    muse_goldrain_set_catcher(&g, 100.0f);
    place_item(&g, MUSE_GOLDRAIN_COIN, 180.0f,
               (float)CATCH_LINE - (float)MUSE_GOLDRAIN_COIN_PX - 2.0f);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS); /* crosses, not caught */
    assert(g.score == 1 && g.caught == 1);
    muse_goldrain_set_catcher(&g, 180.0f); /* slide under, too late */
    tick_n(&g, 20);
    assert(g.score == 1 && g.caught == 1);
    assert(g.missed == 1); /* it fell out the bottom instead */

    /* Miss: a coin falling past the bottom away from the catcher. */
    clear_all(&g);
    muse_goldrain_set_catcher(&g, 180.0f);
    place_item(&g, MUSE_GOLDRAIN_COIN, 80.0f, (float)FIELD_BOTTOM - 20.0f);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS);
    assert(g.missed == 2 && g.score == 1);
    assert(muse_goldrain_falling_count(&g) == 0);

    /* Bomb caught: -1 life. Bomb dropped past the bottom: no penalty. */
    clear_all(&g);
    muse_goldrain_set_catcher(&g, 180.0f);
    place_item(&g, MUSE_GOLDRAIN_BOMB, 180.0f,
               (float)CATCH_LINE - (float)MUSE_GOLDRAIN_COIN_PX - 2.0f);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS);
    assert(g.lives == 2 && g.score == 1);
    clear_all(&g);
    place_item(&g, MUSE_GOLDRAIN_BOMB, 80.0f, (float)FIELD_BOTTOM - 20.0f);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS);
    assert(g.lives == 2);

    /* Third bomb ends the game early. */
    clear_all(&g);
    g.lives = 1;
    muse_goldrain_set_catcher(&g, 180.0f);
    place_item(&g, MUSE_GOLDRAIN_BOMB, 180.0f,
               (float)CATCH_LINE - (float)MUSE_GOLDRAIN_COIN_PX - 2.0f);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS);
    assert(g.phase == MUSE_GOLDRAIN_OVER);
    assert(g.lives == 0);
    assert(muse_goldrain_falling_count(&g) == 0);

    /* Restart after OVER: fresh game, fee is the UI's job. */
    muse_goldrain_restart(&g);
    assert(g.phase == MUSE_GOLDRAIN_PLAYING);
    assert(g.score == 0 && g.lives == MUSE_GOLDRAIN_LIVES);
    assert(muse_goldrain_falling_count(&g) == 1);

    /* Catcher clamps to the field. */
    muse_goldrain_set_catcher(&g, 0.0f);
    assert(fabsf(g.catcher_cx - (60.0f + 53.0f / 2.0f)) < 0.01f);
    muse_goldrain_set_catcher(&g, 9999.0f);
    assert(fabsf(g.catcher_cx - (300.0f - 53.0f / 2.0f)) < 0.01f);

    /* Pause freezes time; resume continues it. */
    uint32_t elapsed = g.elapsed_ms;
    muse_goldrain_pause(&g);
    assert(g.phase == MUSE_GOLDRAIN_PAUSED);
    tick_n(&g, 10);
    assert(g.elapsed_ms == elapsed);
    muse_goldrain_resume(&g);
    assert(g.phase == MUSE_GOLDRAIN_PLAYING);
    tick_n(&g, 10);
    assert(g.elapsed_ms == elapsed + 500);

    /* Speed and gap curves (pure functions, so the floor/cap are
     * checked with elapsed set directly). */
    g.elapsed_ms = 10000;
    assert(fabsf(muse_goldrain_speed_px_s(&g) - 130.0f) < 0.01f);
    assert(muse_goldrain_spawn_gap_ms(&g) == 850);
    g.elapsed_ms = 60000;
    assert(fabsf(muse_goldrain_speed_px_s(&g) - 150.0f) < 0.01f);
    assert(muse_goldrain_spawn_gap_ms(&g) == 600);
    assert(muse_goldrain_remaining_s(&g) == 0);

    /* The 30 s timer ends the round. Bombs cannot end it early here:
     * lives are set high so a random bomb catch does not cut the run. */
    muse_goldrain_init(&g);
    muse_goldrain_start(&g);
    g.lives = 99;
    tick_n(&g, 599); /* 29.95 s, still playing */
    assert(g.phase == MUSE_GOLDRAIN_PLAYING);
    assert(muse_goldrain_remaining_s(&g) == 1);
    muse_goldrain_tick(&g, MUSE_GOLDRAIN_TICK_MS); /* 30.00 s */
    assert(g.phase == MUSE_GOLDRAIN_OVER);
    assert(muse_goldrain_remaining_s(&g) == 0);
    assert(muse_goldrain_falling_count(&g) == 0);

    printf("ok\n");
    return 0;
}

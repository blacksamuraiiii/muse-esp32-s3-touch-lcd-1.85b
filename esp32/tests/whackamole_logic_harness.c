/* Whack-a-mole logic harness: checks muse_whackamole.c without LVGL
 * or ESP-IDF. Run by tests/test_whackamole_logic.py. Prints "ok" on
 * success; any failed check aborts via assert. Deterministic: the
 * random hole picks are seeded once with srand(). */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_whackamole.h"

static int up_hole(const muse_whackamole_t *g)
{
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        if (g->holes[i].state == MUSE_WHACKAMOLE_UP) {
            return i;
        }
    }
    return -1;
}

static void tick_n(muse_whackamole_t *g, int n)
{
    for (int i = 0; i < n; i++) {
        muse_whackamole_tick(g, MUSE_WHACKAMOLE_TICK_MS);
    }
}

int main(void)
{
    srand(12345);

    /* Init state. */
    muse_whackamole_t g;
    muse_whackamole_init(&g);
    assert(g.phase == MUSE_WHACKAMOLE_START);
    assert(g.score == 0 && g.hits == 0 && g.missed == 0);
    assert(g.elapsed_ms == 0);
    assert(muse_whackamole_up_count(&g) == 0);
    assert(muse_whackamole_remaining_s(&g) == 30);

    /* Phase guards before start. */
    muse_whackamole_pause(&g);
    assert(g.phase == MUSE_WHACKAMOLE_START);
    muse_whackamole_resume(&g);
    assert(g.phase == MUSE_WHACKAMOLE_START);
    assert(!muse_whackamole_tap(&g, 0));
    muse_whackamole_tick(&g, 100);
    assert(g.elapsed_ms == 0);

    /* Start: one mole up at once, no other change. */
    muse_whackamole_start(&g);
    assert(g.phase == MUSE_WHACKAMOLE_PLAYING);
    assert(muse_whackamole_up_count(&g) == 1);
    assert(g.elapsed_ms == 0);

    /* Start again: no-op, the game is not reset. */
    int first = up_hole(&g);
    assert(first >= 0);
    muse_whackamole_start(&g);
    assert(g.phase == MUSE_WHACKAMOLE_PLAYING);
    assert(up_hole(&g) == first);

    /* Tap the live mole: hit, score, hit image, then empty. */
    assert(muse_whackamole_tap(&g, first));
    assert(g.holes[first].state == MUSE_WHACKAMOLE_HIT);
    assert(g.hits == 1 && g.score == 1);
    assert(muse_whackamole_up_count(&g) == 0);
    assert(!muse_whackamole_tap(&g, first)); /* already hit */
    assert(g.hits == 1);
    tick_n(&g, 1); /* 100 ms: hit image still showing */
    assert(g.holes[first].state == MUSE_WHACKAMOLE_HIT);
    tick_n(&g, 1); /* 200 ms: hole empties */
    assert(g.holes[first].state == MUSE_WHACKAMOLE_EMPTY);

    /* Tap an empty hole and out-of-range holes: no penalty. */
    assert(!muse_whackamole_tap(&g, first));
    assert(!muse_whackamole_tap(&g, -1));
    assert(!muse_whackamole_tap(&g, MUSE_WHACKAMOLE_HOLES));
    assert(g.hits == 1 && g.score == 1 && g.missed == 0);

    /* Expiry: a mole visible 900 ms at game start runs away at 900. */
    muse_whackamole_restart(&g);
    assert(g.phase == MUSE_WHACKAMOLE_PLAYING);
    assert(g.score == 0 && g.hits == 0 && g.missed == 0);
    int m = up_hole(&g);
    assert(m >= 0);
    assert(g.holes[m].visible_ms == 900);
    tick_n(&g, 8); /* 800 ms: still up */
    assert(g.holes[m].state == MUSE_WHACKAMOLE_UP);
    tick_n(&g, 1); /* 900 ms: runs away; a fresh mole may refill the
     * same hole on this very tick, so only the count is asserted. */
    assert(g.missed == 1);

    /* Remaining seconds count down (rounded up). */
    assert(muse_whackamole_remaining_s(&g) == 30);
    tick_n(&g, 10); /* +1000 ms = 1900 ms elapsed */
    assert(muse_whackamole_remaining_s(&g) == 29);

    /* Pause freezes everything; resume thaws it. */
    uint32_t elapsed_before = g.elapsed_ms;
    muse_whackamole_pause(&g);
    assert(g.phase == MUSE_WHACKAMOLE_PAUSED);
    tick_n(&g, 10);
    assert(g.elapsed_ms == elapsed_before);
    muse_whackamole_resume(&g);
    assert(g.phase == MUSE_WHACKAMOLE_PLAYING);
    tick_n(&g, 1);
    assert(g.elapsed_ms == elapsed_before + 100);

    /* Spawn cap and pacing over a full game: never more than 2 up,
     * fresh moles visible 900 ms early and 500 ms late. */
    muse_whackamole_restart(&g);
    bool saw_early = false, saw_late = false;
    int prev_up[MUSE_WHACKAMOLE_HOLES];
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        prev_up[i] = 0;
    }
    for (int t = 0; t < 300 && g.phase == MUSE_WHACKAMOLE_PLAYING; t++) {
        muse_whackamole_tick(&g, MUSE_WHACKAMOLE_TICK_MS);
        assert(muse_whackamole_up_count(&g) <= MUSE_WHACKAMOLE_MAX_UP);
        for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
            int now_up = (g.holes[i].state == MUSE_WHACKAMOLE_UP);
            if (now_up && !prev_up[i]) {
                if (g.elapsed_ms < 5000) {
                    assert(g.holes[i].visible_ms == 900);
                    saw_early = true;
                }
                if (g.elapsed_ms >= 20000) {
                    assert(g.holes[i].visible_ms == 500);
                    saw_late = true;
                }
            }
            prev_up[i] = now_up;
        }
    }
    assert(saw_early && saw_late);
    assert(g.phase == MUSE_WHACKAMOLE_OVER);
    assert(g.elapsed_ms >= MUSE_WHACKAMOLE_GAME_MS);
    assert(muse_whackamole_up_count(&g) == 0);
    assert(muse_whackamole_remaining_s(&g) == 0);
    for (int i = 0; i < MUSE_WHACKAMOLE_HOLES; i++) {
        assert(g.holes[i].state == MUSE_WHACKAMOLE_EMPTY);
    }

    /* Ticks and taps after OVER are no-ops. */
    uint32_t hits_before = g.hits;
    tick_n(&g, 5);
    assert(g.phase == MUSE_WHACKAMOLE_OVER);
    assert(!muse_whackamole_tap(&g, 0));
    assert(g.hits == hits_before);

    /* Restart from OVER: a fresh game. */
    muse_whackamole_restart(&g);
    assert(g.phase == MUSE_WHACKAMOLE_PLAYING);
    assert(g.score == 0 && g.hits == 0 && g.missed == 0);
    assert(g.elapsed_ms == 0);
    assert(muse_whackamole_up_count(&g) == 1);
    assert(muse_whackamole_remaining_s(&g) == 30);

    printf("ok\n");
    return 0;
}

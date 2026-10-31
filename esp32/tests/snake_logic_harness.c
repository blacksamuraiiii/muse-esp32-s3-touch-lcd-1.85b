/* Snake logic harness: checks muse_snake.c without LVGL or ESP-IDF.
 * Run by tests/test_snake_logic.py. Prints "ok" on success; any failed
 * check aborts via assert. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_snake.h"

static bool food_on_snake(const muse_snake_t *g)
{
    for (int i = 0; i < g->len; i++) {
        if (g->body[i].x == g->food.x && g->body[i].y == g->food.y) {
            return true;
        }
    }
    return false;
}

static void check_food_sane(const muse_snake_t *g)
{
    assert(g->food.x < MUSE_SNAKE_COLS);
    assert(g->food.y < MUSE_SNAKE_ROWS);
    assert(!food_on_snake(g));
}

int main(void)
{
    /* v1 defaults, documented in the header. */
    assert(MUSE_SNAKE_TICK_MS == 200);
    assert(MUSE_SNAKE_COLS == 12);
    assert(MUSE_SNAKE_ROWS == 12);
    assert(MUSE_SNAKE_INIT_LEN == 3);
    assert(MUSE_SNAKE_FOOD_SCORE == 10);

    srand(1234);

    /* Init: START state, length 3 moving right from the centre. */
    muse_snake_t g;
    muse_snake_init(&g);
    assert(g.state == MUSE_SNAKE_START);
    assert(g.len == 3);
    assert(g.score == 0);
    assert(g.dir == MUSE_SNAKE_RIGHT);
    assert(g.body[0].x == 6 && g.body[0].y == 5);
    assert(g.body[1].x == 5 && g.body[1].y == 5);
    assert(g.body[2].x == 4 && g.body[2].y == 5);
    check_food_sane(&g);

    /* Tick is a no-op outside PLAYING; set_dir is ignored too. */
    muse_snake_tick(&g);
    assert(g.body[0].x == 6 && g.body[0].y == 5);
    muse_snake_set_dir(&g, MUSE_SNAKE_DOWN);
    muse_snake_start(&g);
    assert(g.state == MUSE_SNAKE_PLAYING);
    muse_snake_tick(&g);
    /* The START-state turn was ignored: still moving right. */
    assert(g.body[0].x == 7 && g.body[0].y == 5);
    assert(g.body[1].x == 6 && g.body[1].y == 5);
    assert(g.body[2].x == 5 && g.body[2].y == 5);

    /* Turn: DOWN applies at the next tick. */
    muse_snake_set_dir(&g, MUSE_SNAKE_DOWN);
    muse_snake_tick(&g);
    assert(g.body[0].x == 7 && g.body[0].y == 6);
    assert(g.dir == MUSE_SNAKE_DOWN);

    /* Reversal is ignored: DOWN then UP keeps DOWN. */
    muse_snake_set_dir(&g, MUSE_SNAKE_UP);
    muse_snake_tick(&g);
    assert(g.body[0].x == 7 && g.body[0].y == 7);
    assert(g.dir == MUSE_SNAKE_DOWN);

    /* One turn per tick, last VALID wins: moving down, LEFT then
     * RIGHT are both valid, so the later one (RIGHT) applies; and
     * moving right, UP then LEFT resolves to UP because LEFT is the
     * opposite of the current direction and is dropped. */
    muse_snake_set_dir(&g, MUSE_SNAKE_LEFT);
    muse_snake_set_dir(&g, MUSE_SNAKE_RIGHT);
    muse_snake_tick(&g);
    assert(g.dir == MUSE_SNAKE_RIGHT);
    assert(g.body[0].x == 8 && g.body[0].y == 7);
    muse_snake_set_dir(&g, MUSE_SNAKE_UP);
    muse_snake_set_dir(&g, MUSE_SNAKE_LEFT);
    muse_snake_tick(&g);
    assert(g.dir == MUSE_SNAKE_UP);
    assert(g.body[0].x == 8 && g.body[0].y == 6);

    /* Eat: food ahead grows the snake and scores 10, new food is sane. */
    muse_snake_t e;
    muse_snake_init(&e);
    assert(muse_snake_set_food(&e, 7, 5));
    muse_snake_start(&e);
    muse_snake_tick(&e);
    assert(e.body[0].x == 7 && e.body[0].y == 5);
    assert(e.len == 4);
    assert(e.score == 10);
    check_food_sane(&e);

    /* set_food refusals: out of bounds and on the snake. */
    assert(!muse_snake_set_food(&e, 12, 5));
    assert(!muse_snake_set_food(&e, 7, 5));
    assert(e.food.x != 7 || e.food.y != 5);

    /* Random placement never lands on the snake or outside the board. */
    for (int i = 0; i < 500; i++) {
        muse_snake_t f;
        muse_snake_init(&f);
        check_food_sane(&f);
    }

    /* Pause / resume: ticks stop while paused, continue after resume. */
    muse_snake_pause(&e);
    assert(e.state == MUSE_SNAKE_PAUSED);
    muse_snake_cell_t head = e.body[0];
    muse_snake_tick(&e);
    assert(e.body[0].x == head.x && e.body[0].y == head.y);
    muse_snake_resume(&e);
    assert(e.state == MUSE_SNAKE_PLAYING);
    muse_snake_tick(&e);
    assert(e.body[0].x != head.x || e.body[0].y != head.y);
    /* Pause / resume are no-ops in the wrong state. */
    muse_snake_resume(&e);
    assert(e.state == MUSE_SNAKE_PLAYING);
    muse_snake_pause(&e);
    muse_snake_pause(&e);
    assert(e.state == MUSE_SNAKE_PAUSED);

    /* Wall, right edge: drive to x = 11, one more tick ends the game. */
    muse_snake_t w;
    muse_snake_init(&w);
    muse_snake_start(&w);
    for (int i = 0; i < 5; i++) {
        muse_snake_tick(&w);
    }
    assert(w.body[0].x == 11 && w.body[0].y == 5);
    assert(w.state == MUSE_SNAKE_PLAYING);
    muse_snake_tick(&w);
    assert(w.state == MUSE_SNAKE_OVER);
    /* Ticks after OVER change nothing. */
    muse_snake_tick(&w);
    assert(w.state == MUSE_SNAKE_OVER);

    /* Wall, top-left corner: up to y = 0, left to x = 0, then out. */
    muse_snake_t c;
    muse_snake_init(&c);
    muse_snake_start(&c);
    muse_snake_set_dir(&c, MUSE_SNAKE_UP);
    for (int i = 0; i < 5; i++) {
        muse_snake_tick(&c);
    }
    assert(c.body[0].x == 6 && c.body[0].y == 0);
    muse_snake_set_dir(&c, MUSE_SNAKE_LEFT);
    for (int i = 0; i < 6; i++) {
        muse_snake_tick(&c);
    }
    assert(c.body[0].x == 0 && c.body[0].y == 0);
    muse_snake_tick(&c);
    assert(c.state == MUSE_SNAKE_OVER);

    /* Self collision, scripted: grow to 6, loop back into the body.
     * Food is pinned to (0,0) before the last two moves so the random
     * placement cannot land on the collision cell. */
    muse_snake_t s;
    muse_snake_init(&s);
    assert(muse_snake_set_food(&s, 7, 5));
    muse_snake_start(&s);
    muse_snake_tick(&s); /* eat -> len 4 */
    assert(muse_snake_set_food(&s, 8, 5));
    muse_snake_tick(&s); /* eat -> len 5 */
    muse_snake_set_dir(&s, MUSE_SNAKE_DOWN);
    assert(muse_snake_set_food(&s, 8, 6));
    muse_snake_tick(&s); /* eat -> len 6 */
    assert(s.len == 6 && s.score == 30);
    assert(muse_snake_set_food(&s, 0, 0));
    muse_snake_set_dir(&s, MUSE_SNAKE_LEFT);
    muse_snake_tick(&s);
    assert(s.body[0].x == 7 && s.body[0].y == 6);
    assert(s.state == MUSE_SNAKE_PLAYING);
    muse_snake_set_dir(&s, MUSE_SNAKE_UP);
    muse_snake_tick(&s);
    assert(s.state == MUSE_SNAKE_OVER);

    /* Moving into the cell the tail just vacated is legal. */
    muse_snake_t t;
    muse_snake_init(&t);
    assert(muse_snake_set_food(&t, 7, 5));
    muse_snake_start(&t);
    muse_snake_tick(&t); /* len 4: [(7,5),(6,5),(5,5),(4,5)] */
    assert(muse_snake_set_food(&t, 0, 0)); /* pin food away from the path */
    muse_snake_set_dir(&t, MUSE_SNAKE_DOWN);
    muse_snake_tick(&t); /* [(7,6),(7,5),(6,5),(5,5)] */
    muse_snake_set_dir(&t, MUSE_SNAKE_LEFT);
    muse_snake_tick(&t); /* [(6,6),(7,6),(7,5),(6,5)] */
    muse_snake_set_dir(&t, MUSE_SNAKE_UP);
    muse_snake_tick(&t); /* head (6,5): the tail just left it, legal */
    assert(t.state == MUSE_SNAKE_PLAYING);
    assert(t.body[0].x == 6 && t.body[0].y == 5);

    /* Restart: any state -> a fresh PLAYING game. */
    muse_snake_restart(&w);
    assert(w.state == MUSE_SNAKE_PLAYING);
    assert(w.len == 3 && w.score == 0);
    assert(w.dir == MUSE_SNAKE_RIGHT);
    assert(w.body[0].x == 6 && w.body[0].y == 5);
    check_food_sane(&w);

    printf("ok\n");
    return 0;
}

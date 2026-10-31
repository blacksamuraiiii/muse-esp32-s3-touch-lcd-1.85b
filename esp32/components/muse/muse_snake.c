/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_snake.c -- Snake logic, see muse_snake.h.
 *
 * No LVGL, no ESP-IDF: only stdlib rand() for food placement, seeded
 * by the caller (see muse_snake_init).
 */
#include "muse_snake.h"

#include <stdlib.h>

static bool is_opposite(muse_snake_dir_t a, muse_snake_dir_t b)
{
    return (a == MUSE_SNAKE_UP && b == MUSE_SNAKE_DOWN) ||
           (a == MUSE_SNAKE_DOWN && b == MUSE_SNAKE_UP) ||
           (a == MUSE_SNAKE_LEFT && b == MUSE_SNAKE_RIGHT) ||
           (a == MUSE_SNAKE_RIGHT && b == MUSE_SNAKE_LEFT);
}

static bool on_snake(const muse_snake_t *g, uint8_t x, uint8_t y)
{
    for (int i = 0; i < g->len; i++) {
        if (g->body[i].x == x && g->body[i].y == y) {
            return true;
        }
    }
    return false;
}

/* Random free cell, or false when the board is full. Only the caller
 * (tick, after an eat) can reach a full board: init always has room. */
static bool place_food(muse_snake_t *g)
{
    muse_snake_cell_t free_cells[MUSE_SNAKE_CELLS];
    int n = 0;
    for (int y = 0; y < MUSE_SNAKE_ROWS; y++) {
        for (int x = 0; x < MUSE_SNAKE_COLS; x++) {
            if (!on_snake(g, (uint8_t)x, (uint8_t)y)) {
                free_cells[n].x = (uint8_t)x;
                free_cells[n].y = (uint8_t)y;
                n++;
            }
        }
    }
    if (n == 0) {
        return false;
    }
    g->food = free_cells[rand() % n];
    return true;
}

void muse_snake_init(muse_snake_t *g)
{
    g->state = MUSE_SNAKE_START;
    g->dir = MUSE_SNAKE_RIGHT;
    g->score = 0;
    g->len = MUSE_SNAKE_INIT_LEN;
    g->body[0].x = 6;
    g->body[0].y = 5;
    g->body[1].x = 5;
    g->body[1].y = 5;
    g->body[2].x = 4;
    g->body[2].y = 5;
    g->has_pending = false;
    g->pending = MUSE_SNAKE_RIGHT;
    place_food(g);
}

void muse_snake_start(muse_snake_t *g)
{
    if (g->state == MUSE_SNAKE_START) {
        g->state = MUSE_SNAKE_PLAYING;
    }
}

void muse_snake_pause(muse_snake_t *g)
{
    if (g->state == MUSE_SNAKE_PLAYING) {
        g->state = MUSE_SNAKE_PAUSED;
    }
}

void muse_snake_resume(muse_snake_t *g)
{
    if (g->state == MUSE_SNAKE_PAUSED) {
        g->state = MUSE_SNAKE_PLAYING;
    }
}

void muse_snake_restart(muse_snake_t *g)
{
    muse_snake_init(g);
    g->state = MUSE_SNAKE_PLAYING;
}

void muse_snake_set_dir(muse_snake_t *g, muse_snake_dir_t dir)
{
    if (g->state != MUSE_SNAKE_PLAYING) {
        return;
    }
    if (is_opposite(dir, g->dir)) {
        return;
    }
    g->pending = dir;
    g->has_pending = true;
}

void muse_snake_tick(muse_snake_t *g)
{
    if (g->state != MUSE_SNAKE_PLAYING) {
        return;
    }
    if (g->has_pending) {
        g->dir = g->pending;
        g->has_pending = false;
    }
    int nx = g->body[0].x;
    int ny = g->body[0].y;
    switch (g->dir) {
    case MUSE_SNAKE_UP: ny--; break;
    case MUSE_SNAKE_DOWN: ny++; break;
    case MUSE_SNAKE_LEFT: nx--; break;
    case MUSE_SNAKE_RIGHT: nx++; break;
    }
    if (nx < 0 || nx >= MUSE_SNAKE_COLS || ny < 0 || ny >= MUSE_SNAKE_ROWS) {
        g->state = MUSE_SNAKE_OVER;
        return;
    }
    bool eat = (nx == g->food.x && ny == g->food.y);
    if (eat && g->len >= MUSE_SNAKE_CELLS) {
        /* Defensive only: food always sits on a free cell, so the snake
         * can never eat while already filling the board. */
        g->state = MUSE_SNAKE_OVER;
        return;
    }
    /* The tail vacates unless the snake eats, so the body checked here
     * excludes the tail on a plain move. */
    int check = eat ? g->len : g->len - 1;
    for (int i = 0; i < check; i++) {
        if (g->body[i].x == nx && g->body[i].y == ny) {
            g->state = MUSE_SNAKE_OVER;
            return;
        }
    }
    int new_len = eat ? g->len + 1 : g->len;
    for (int i = new_len - 1; i > 0; i--) {
        g->body[i] = g->body[i - 1];
    }
    g->body[0].x = (uint8_t)nx;
    g->body[0].y = (uint8_t)ny;
    g->len = (uint8_t)new_len;
    if (eat) {
        g->score += MUSE_SNAKE_FOOD_SCORE;
        if (!place_food(g)) {
            /* Board full after this eat: no cell left for new food. */
            g->state = MUSE_SNAKE_OVER;
        }
    }
}

bool muse_snake_set_food(muse_snake_t *g, uint8_t x, uint8_t y)
{
    if (x >= MUSE_SNAKE_COLS || y >= MUSE_SNAKE_ROWS) {
        return false;
    }
    if (on_snake(g, x, y)) {
        return false;
    }
    g->food.x = x;
    g->food.y = y;
    return true;
}

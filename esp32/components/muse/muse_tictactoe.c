/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tictactoe.c -- OX棋 logic, see muse_tictactoe.h.
 *
 * The board is cells[9], row-major, EMPTY / X (player) / O (AI).
 * A move ends the game at once when it takes a line or fills the
 * board; the outcome and the winning line are stored on the struct
 * so the UI can draw the gold line without re-deriving it.
 *
 * The AI is easy on purpose (user decision 2026-11-08, beatable by a
 * 7-year-old): it takes a win when one is open, blocks the player's
 * immediate win MUSE_TICTACTOE_BLOCK_PCT percent of the time, and
 * otherwise picks a random empty cell. The missed block is the
 * player's opening -- the AI never plays a perfect game.
 */
#include "muse_tictactoe.h"

#include <stdlib.h>

/* The 8 lines: rows, columns, diagonals. */
static const uint8_t k_lines[8][3] = {
    {0, 1, 2}, {3, 4, 5}, {6, 7, 8},
    {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
    {0, 4, 8}, {2, 4, 6},
};

/* The line `mark` would complete by playing `cell`, or -1. */
static int completing_line(const muse_tictactoe_t *g,
                           muse_tictactoe_mark_t mark, int cell)
{
    for (int i = 0; i < 8; i++) {
        int on_line = 0;
        int count = 0;
        for (int j = 0; j < 3; j++) {
            int c = k_lines[i][j];
            if (c == cell) {
                on_line = 1;
            } else if (g->cells[c] == mark) {
                count++;
            } else if (g->cells[c] != MUSE_TICTACTOE_EMPTY) {
                count = -1;
                break;
            }
        }
        if (on_line && count == 2) {
            return i;
        }
    }
    return -1;
}

static int empty_count(const muse_tictactoe_t *g)
{
    int n = 0;
    for (int i = 0; i < MUSE_TICTACTOE_CELLS; i++) {
        if (g->cells[i] == MUSE_TICTACTOE_EMPTY) {
            n++;
        }
    }
    return n;
}

/* Settle the game after a move: a line wins it, a full board draws
 * it, anything else leaves it playing. */
static void settle_after_move(muse_tictactoe_t *g)
{
    int line = muse_tictactoe_win_line(g);
    if (line >= 0) {
        g->win_line = line;
        g->outcome = (muse_tictactoe_winner(g) == MUSE_TICTACTOE_X)
                         ? MUSE_TICTACTOE_WIN
                         : MUSE_TICTACTOE_LOSS;
        g->phase = MUSE_TICTACTOE_OVER;
        g->ai_turn = false;
        return;
    }
    if (muse_tictactoe_board_full(g)) {
        g->outcome = MUSE_TICTACTOE_DRAW;
        g->phase = MUSE_TICTACTOE_OVER;
        g->ai_turn = false;
    }
}

void muse_tictactoe_init(muse_tictactoe_t *g)
{
    g->phase = MUSE_TICTACTOE_START;
    for (int i = 0; i < MUSE_TICTACTOE_CELLS; i++) {
        g->cells[i] = MUSE_TICTACTOE_EMPTY;
    }
    g->outcome = MUSE_TICTACTOE_NONE;
    g->win_line = -1;
    g->ai_turn = false;
}

void muse_tictactoe_start(muse_tictactoe_t *g)
{
    if (g->phase != MUSE_TICTACTOE_START) {
        return;
    }
    muse_tictactoe_init(g);
    g->phase = MUSE_TICTACTOE_PLAYING;
}

void muse_tictactoe_pause(muse_tictactoe_t *g)
{
    if (g->phase == MUSE_TICTACTOE_PLAYING) {
        g->phase = MUSE_TICTACTOE_PAUSED;
    }
}

void muse_tictactoe_resume(muse_tictactoe_t *g)
{
    if (g->phase == MUSE_TICTACTOE_PAUSED) {
        g->phase = MUSE_TICTACTOE_PLAYING;
    }
}

void muse_tictactoe_restart(muse_tictactoe_t *g)
{
    muse_tictactoe_init(g);
    g->phase = MUSE_TICTACTOE_PLAYING;
}

bool muse_tictactoe_play(muse_tictactoe_t *g, int cell)
{
    if (g->phase != MUSE_TICTACTOE_PLAYING || g->ai_turn) {
        return false;
    }
    if (cell < 0 || cell >= MUSE_TICTACTOE_CELLS) {
        return false;
    }
    if (g->cells[cell] != MUSE_TICTACTOE_EMPTY) {
        return false;
    }
    g->cells[cell] = MUSE_TICTACTOE_X;
    settle_after_move(g);
    if (g->phase == MUSE_TICTACTOE_PLAYING) {
        g->ai_turn = true;
    }
    return true;
}

int muse_tictactoe_ai_move(muse_tictactoe_t *g)
{
    if (g->phase != MUSE_TICTACTOE_PLAYING || !g->ai_turn) {
        return -1;
    }
    g->ai_turn = false;
    int n = empty_count(g);
    if (n == 0) {
        return -1;
    }
    int cell = -1;
    /* 1. Take a win when one is open. */
    for (int i = 0; i < MUSE_TICTACTOE_CELLS && cell < 0; i++) {
        if (g->cells[i] == MUSE_TICTACTOE_EMPTY &&
            completing_line(g, MUSE_TICTACTOE_O, i) >= 0) {
            cell = i;
        }
    }
    /* 2. Block the player's immediate win -- usually. The missed
     * block is the player's opening: rand() % 100 rolls under
     * BLOCK_PCT to block, over it to let the win through. */
    if (cell < 0 && (rand() % 100) < MUSE_TICTACTOE_BLOCK_PCT) {
        for (int i = 0; i < MUSE_TICTACTOE_CELLS && cell < 0; i++) {
            if (g->cells[i] == MUSE_TICTACTOE_EMPTY &&
                completing_line(g, MUSE_TICTACTOE_X, i) >= 0) {
                cell = i;
            }
        }
    }
    /* 3. Otherwise a random empty cell. */
    if (cell < 0) {
        int pick = rand() % n;
        for (int i = 0; i < MUSE_TICTACTOE_CELLS; i++) {
            if (g->cells[i] == MUSE_TICTACTOE_EMPTY) {
                if (pick == 0) {
                    cell = i;
                    break;
                }
                pick--;
            }
        }
    }
    if (cell < 0) {
        return -1;
    }
    g->cells[cell] = MUSE_TICTACTOE_O;
    settle_after_move(g);
    return cell;
}

int muse_tictactoe_winner(const muse_tictactoe_t *g)
{
    for (int i = 0; i < 8; i++) {
        muse_tictactoe_mark_t m = g->cells[k_lines[i][0]];
        if (m != MUSE_TICTACTOE_EMPTY && m == g->cells[k_lines[i][1]] &&
            m == g->cells[k_lines[i][2]]) {
            return m;
        }
    }
    return MUSE_TICTACTOE_EMPTY;
}

int muse_tictactoe_win_line(const muse_tictactoe_t *g)
{
    for (int i = 0; i < 8; i++) {
        muse_tictactoe_mark_t m = g->cells[k_lines[i][0]];
        if (m != MUSE_TICTACTOE_EMPTY && m == g->cells[k_lines[i][1]] &&
            m == g->cells[k_lines[i][2]]) {
            return i;
        }
    }
    return -1;
}

bool muse_tictactoe_board_full(const muse_tictactoe_t *g)
{
    return empty_count(g) == 0;
}

void muse_tictactoe_line_cells(int line, int *first, int *last)
{
    if (line < 0 || line >= 8) {
        if (first) {
            *first = -1;
        }
        if (last) {
            *last = -1;
        }
        return;
    }
    if (first) {
        *first = k_lines[line][0];
    }
    if (last) {
        *last = k_lines[line][2];
    }
}

uint32_t muse_tictactoe_payout_coins(muse_tictactoe_outcome_t outcome)
{
    switch (outcome) {
        case MUSE_TICTACTOE_WIN:
            return MUSE_TICTACTOE_PAYOUT_WIN;
        case MUSE_TICTACTOE_DRAW:
            return MUSE_TICTACTOE_PAYOUT_DRAW;
        default:
            return MUSE_TICTACTOE_PAYOUT_LOSS;
    }
}

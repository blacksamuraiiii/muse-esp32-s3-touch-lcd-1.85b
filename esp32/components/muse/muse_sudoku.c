/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_sudoku.c -- 6x6 Sudoku logic, see muse_sudoku.h.
 *
 * Puzzle data: 20 puzzles with verified unique solutions, generated
 * 2026-10-31 (seed 20261031) and checked by counting solutions up to 2
 * per puzzle. Difficulty bands by givens: #01-07 easy (24), #08-14
 * medium (21), #15-20 hard (18). The generator script is not shipped;
 * the tables below are its output, copied verbatim. No puzzle content
 * is taken from any published puzzle source.
 */
#include "muse_sudoku.h"

static const uint8_t s_puzzles[MUSE_SUDOKU_COUNT][MUSE_SUDOKU_CELLS] = {
    /* #01 */ {0,0,0,0,1,0,1,2,6,4,3,0,3,4,1,5,2,6,2,0,5,1,4,3,4,0,2,3,0,0,6,1,0,2,0,4},
    /* #02 */ {6,1,2,0,0,3,5,4,0,0,1,2,0,5,6,0,0,1,2,3,1,0,5,6,3,0,0,1,2,5,0,0,5,3,6,4},
    /* #03 */ {0,5,6,4,3,0,3,0,4,1,5,6,4,0,5,2,0,3,6,3,2,0,4,0,5,0,1,3,0,4,0,4,0,6,1,0},
    /* #04 */ {0,6,2,0,0,4,5,3,4,6,1,2,3,0,0,1,4,5,0,1,5,3,0,0,0,5,3,4,0,1,0,4,0,2,5,3},
    /* #05 */ {6,1,3,0,0,2,2,5,0,6,1,0,0,0,2,1,3,6,1,3,6,0,4,5,3,2,0,5,0,4,4,6,0,0,0,1},
    /* #06 */ {0,4,5,3,1,6,0,6,3,0,2,4,5,1,6,0,4,3,0,2,4,0,5,0,4,0,0,1,0,5,6,0,1,4,3,0},
    /* #07 */ {3,6,5,0,2,4,0,4,2,0,0,0,6,5,4,2,0,1,2,1,3,6,0,0,4,0,1,0,5,0,0,3,6,4,1,2},
    /* #08 */ {5,0,2,6,0,1,1,0,0,2,0,5,0,1,6,0,2,4,0,0,0,0,1,0,2,5,4,1,0,3,6,3,1,4,0,0},
    /* #09 */ {2,6,0,0,3,0,3,4,0,0,1,0,4,0,0,1,2,0,0,3,0,4,5,0,5,2,4,3,0,1,6,1,3,2,0,0},
    /* #10 */ {4,0,0,0,0,0,6,3,5,0,2,0,0,1,0,4,0,2,2,5,4,6,0,1,0,6,0,0,4,3,5,4,3,2,0,6},
    /* #11 */ {0,4,0,1,5,6,5,1,6,3,4,0,0,0,0,0,0,3,3,0,2,5,1,0,0,3,0,4,0,1,4,0,0,6,3,5},
    /* #12 */ {6,0,0,4,1,5,5,0,1,2,3,6,0,3,6,0,4,0,2,5,0,3,0,1,3,0,5,1,0,0,0,1,0,6,0,0},
    /* #13 */ {0,4,1,2,0,0,0,3,5,4,0,1,1,0,6,3,0,4,3,5,4,6,0,0,0,0,2,1,4,0,4,1,3,0,0,0},
    /* #14 */ {0,4,1,0,0,6,2,0,3,0,0,4,4,0,0,0,1,3,3,0,6,5,4,0,0,5,4,0,3,1,0,3,2,4,6,0},
    /* #15 */ {0,6,5,1,0,2,0,0,0,0,3,5,0,1,0,0,0,4,4,5,0,2,0,1,0,0,1,4,0,0,0,2,0,5,1,3},
    /* #16 */ {0,0,0,0,3,0,6,2,3,1,5,0,3,4,0,0,1,0,2,0,5,4,6,0,0,0,0,0,0,1,0,3,2,6,4,0},
    /* #17 */ {5,0,0,0,1,0,0,0,2,0,4,5,0,0,0,6,2,1,0,0,6,4,5,3,0,0,0,1,3,2,0,3,0,0,6,4},
    /* #18 */ {0,2,0,1,0,5,0,4,5,2,0,6,0,0,0,5,6,2,2,0,6,0,0,3,3,0,0,0,0,0,5,0,4,3,0,1},
    /* #19 */ {0,1,2,0,0,0,0,4,0,3,0,2,4,5,1,2,3,6,6,0,0,1,0,0,1,0,5,4,0,0,0,0,0,0,5,1},
    /* #20 */ {4,2,6,0,5,0,0,1,0,2,0,0,0,0,4,5,0,3,0,0,3,6,1,0,0,3,0,4,6,2,6,4,0,0,0,0},
};

static const uint8_t s_solutions[MUSE_SUDOKU_COUNT][MUSE_SUDOKU_CELLS] = {
    /* #01 */ {5,3,4,6,1,2,1,2,6,4,3,5,3,4,1,5,2,6,2,6,5,1,4,3,4,5,2,3,6,1,6,1,3,2,5,4},
    /* #02 */ {6,1,2,5,4,3,5,4,3,6,1,2,4,5,6,2,3,1,2,3,1,4,5,6,3,6,4,1,2,5,1,2,5,3,6,4},
    /* #03 */ {1,5,6,4,3,2,3,2,4,1,5,6,4,1,5,2,6,3,6,3,2,5,4,1,5,6,1,3,2,4,2,4,3,6,1,5},
    /* #04 */ {1,6,2,5,3,4,5,3,4,6,1,2,3,2,6,1,4,5,4,1,5,3,2,6,2,5,3,4,6,1,6,4,1,2,5,3},
    /* #05 */ {6,1,3,4,5,2,2,5,4,6,1,3,5,4,2,1,3,6,1,3,6,2,4,5,3,2,1,5,6,4,4,6,5,3,2,1},
    /* #06 */ {2,4,5,3,1,6,1,6,3,5,2,4,5,1,6,2,4,3,3,2,4,6,5,1,4,3,2,1,6,5,6,5,1,4,3,2},
    /* #07 */ {3,6,5,1,2,4,1,4,2,5,6,3,6,5,4,2,3,1,2,1,3,6,4,5,4,2,1,3,5,6,5,3,6,4,1,2},
    /* #08 */ {5,4,2,6,3,1,1,6,3,2,4,5,3,1,6,5,2,4,4,2,5,3,1,6,2,5,4,1,6,3,6,3,1,4,5,2},
    /* #09 */ {2,6,1,5,3,4,3,4,5,6,1,2,4,5,6,1,2,3,1,3,2,4,5,6,5,2,4,3,6,1,6,1,3,2,4,5},
    /* #10 */ {4,2,1,3,6,5,6,3,5,1,2,4,3,1,6,4,5,2,2,5,4,6,3,1,1,6,2,5,4,3,5,4,3,2,1,6},
    /* #11 */ {2,4,3,1,5,6,5,1,6,3,4,2,1,5,4,2,6,3,3,6,2,5,1,4,6,3,5,4,2,1,4,2,1,6,3,5},
    /* #12 */ {6,2,3,4,1,5,5,4,1,2,3,6,1,3,6,5,4,2,2,5,4,3,6,1,3,6,5,1,2,4,4,1,2,6,5,3},
    /* #13 */ {6,4,1,2,3,5,2,3,5,4,6,1,1,2,6,3,5,4,3,5,4,6,1,2,5,6,2,1,4,3,4,1,3,5,2,6},
    /* #14 */ {5,4,1,3,2,6,2,6,3,1,5,4,4,2,5,6,1,3,3,1,6,5,4,2,6,5,4,2,3,1,1,3,2,4,6,5},
    /* #15 */ {3,6,5,1,4,2,1,4,2,6,3,5,2,1,6,3,5,4,4,5,3,2,6,1,5,3,1,4,2,6,6,2,4,5,1,3},
    /* #16 */ {4,5,1,2,3,6,6,2,3,1,5,4,3,4,6,5,1,2,2,1,5,4,6,3,5,6,4,3,2,1,1,3,2,6,4,5},
    /* #17 */ {5,4,3,2,1,6,6,1,2,3,4,5,3,5,4,6,2,1,1,2,6,4,5,3,4,6,5,1,3,2,2,3,1,5,6,4},
    /* #18 */ {6,2,3,1,4,5,1,4,5,2,3,6,4,3,1,5,6,2,2,5,6,4,1,3,3,1,2,6,5,4,5,6,4,3,2,1},
    /* #19 */ {3,1,2,5,6,4,5,4,6,3,1,2,4,5,1,2,3,6,6,2,3,1,4,5,1,6,5,4,2,3,2,3,4,6,5,1},
    /* #20 */ {4,2,6,3,5,1,3,1,5,2,4,6,1,6,4,5,2,3,2,5,3,6,1,4,5,3,1,4,6,2,6,4,2,1,3,5},
};

static int box_of(int r, int c)
{
    return (r / 2) * 3 + (c / 3);
}

/* True when val at pos duplicates another cell in its row, column or
 * box. pos itself is excluded. */
static bool duplicates(const muse_sudoku_state_t *st, int pos, uint8_t val)
{
    int r = pos / MUSE_SUDOKU_SIZE;
    int c = pos % MUSE_SUDOKU_SIZE;
    int b = box_of(r, c);
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        if (i == pos || st->grid[i] != val) {
            continue;
        }
        int ir = i / MUSE_SUDOKU_SIZE;
        int ic = i % MUSE_SUDOKU_SIZE;
        if (ir == r || ic == c || box_of(ir, ic) == b) {
            return true;
        }
    }
    return false;
}

static void recompute_errors(muse_sudoku_state_t *st)
{
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        st->err[i] = (st->grid[i] != 0 && duplicates(st, i, st->grid[i])) ? 1 : 0;
    }
}

int muse_sudoku_count(void)
{
    return MUSE_SUDOKU_COUNT;
}

void muse_sudoku_load(muse_sudoku_state_t *st, int idx)
{
    idx %= MUSE_SUDOKU_COUNT;
    if (idx < 0) {
        idx += MUSE_SUDOKU_COUNT;
    }
    st->idx = idx;
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        st->grid[i] = s_puzzles[idx][i];
        st->given[i] = (s_puzzles[idx][i] != 0) ? 1 : 0;
        st->err[i] = 0;
    }
}

bool muse_sudoku_is_given(const muse_sudoku_state_t *st, int pos)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS) {
        return false;
    }
    return st->given[pos] != 0;
}

uint8_t muse_sudoku_value(const muse_sudoku_state_t *st, int pos)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS) {
        return 0;
    }
    return st->grid[pos];
}

bool muse_sudoku_is_error(const muse_sudoku_state_t *st, int pos)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS) {
        return false;
    }
    return st->err[pos] != 0;
}

uint8_t muse_sudoku_solution(const muse_sudoku_state_t *st, int pos)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS) {
        return 0;
    }
    return s_solutions[st->idx][pos];
}

bool muse_sudoku_set(muse_sudoku_state_t *st, int pos, uint8_t val)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS || val < 1 || val > 6) {
        return false;
    }
    if (st->given[pos]) {
        return false;
    }
    st->grid[pos] = val;
    recompute_errors(st);
    return true;
}

void muse_sudoku_clear(muse_sudoku_state_t *st, int pos)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS || st->given[pos]) {
        return;
    }
    st->grid[pos] = 0;
    recompute_errors(st);
}

bool muse_sudoku_hint(muse_sudoku_state_t *st, int pos)
{
    if (pos < 0 || pos >= MUSE_SUDOKU_CELLS || st->given[pos]) {
        return false;
    }
    uint8_t sol = s_solutions[st->idx][pos];
    if (st->grid[pos] == sol) {
        return false;
    }
    st->grid[pos] = sol;
    recompute_errors(st);
    return true;
}

int muse_sudoku_first_empty(const muse_sudoku_state_t *st)
{
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        if (st->grid[i] == 0) {
            return i;
        }
    }
    return -1;
}

bool muse_sudoku_is_complete(const muse_sudoku_state_t *st)
{
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        if (st->grid[i] == 0 || st->err[i] != 0) {
            return false;
        }
    }
    return true;
}

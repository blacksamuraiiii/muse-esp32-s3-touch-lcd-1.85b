/* Sudoku logic harness: checks muse_sudoku.c without LVGL or ESP-IDF.
 * Run by tests/test_sudoku_logic.py. Prints "ok" on success; any failed
 * check aborts via assert. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "muse_sudoku.h"

/* Backtracking solver that counts solutions up to `limit`. Operates on a
 * plain 36-byte grid (0 = empty); independent of the firmware logic, so
 * the uniqueness check does not just re-ask the code under test. */
static bool solver_ok(const uint8_t *g, int pos, int *count, int limit)
{
    while (pos < 36 && g[pos] != 0) {
        pos++;
    }
    if (pos >= 36) {
        (*count)++;
        return *count >= limit;
    }
    int r = pos / 6, c = pos % 6;
    for (int v = 1; v <= 6; v++) {
        bool bad = false;
        for (int i = 0; i < 36 && !bad; i++) {
            if (g[i] != v) {
                continue;
            }
            int ir = i / 6, ic = i % 6;
            if (ir == r || ic == c || (ir / 2 == r / 2 && ic / 3 == c / 3)) {
                bad = true;
            }
        }
        if (bad) {
            continue;
        }
        uint8_t next[36];
        memcpy(next, g, sizeof(next));
        next[pos] = (uint8_t)v;
        if (solver_ok(next, pos + 1, count, limit)) {
            return true;
        }
    }
    return false;
}

static int count_solutions(const uint8_t *givens)
{
    int count = 0;
    solver_ok(givens, 0, &count, 2);
    return count;
}

int main(void)
{
    assert(muse_sudoku_count() == 20);

    for (int idx = 0; idx < 20; idx++) {
        muse_sudoku_state_t st;
        muse_sudoku_load(&st, idx);
        assert(st.idx == idx);
        int givens = 0;
        for (int i = 0; i < 36; i++) {
            if (st.given[i]) {
                givens++;
                assert(st.grid[i] != 0);
                assert(st.err[i] == 0);
            }
        }
        int want = (idx < 7) ? 24 : (idx < 14) ? 21 : 18;
        assert(givens == want);
        assert(!muse_sudoku_is_complete(&st));
        assert(muse_sudoku_first_empty(&st) >= 0);

        /* Solution respects the givens and completes the board. */
        for (int i = 0; i < 36; i++) {
            if (st.given[i]) {
                assert(muse_sudoku_solution(&st, i) == st.grid[i]);
            }
        }
        for (int i = 0; i < 36; i++) {
            if (!st.given[i]) {
                assert(muse_sudoku_set(&st, i, muse_sudoku_solution(&st, i)));
            }
        }
        assert(muse_sudoku_is_complete(&st));
        assert(muse_sudoku_first_empty(&st) == -1);

        /* Each puzzle has exactly one solution (counted up to 2). */
        muse_sudoku_load(&st, idx);
        assert(count_solutions(st.grid) == 1);
    }

    /* Set / conflict / clear on puzzle #01. */
    muse_sudoku_state_t st;
    muse_sudoku_load(&st, 0);
    int empty = muse_sudoku_first_empty(&st);
    assert(empty >= 0);
    int er = empty / 6;
    int clue = -1;
    for (int c = 0; c < 6; c++) {
        if (st.given[er * 6 + c]) {
            clue = er * 6 + c;
            break;
        }
    }
    assert(clue >= 0);
    uint8_t dup = st.grid[clue];
    assert(muse_sudoku_set(&st, empty, dup));
    assert(st.grid[empty] == dup);
    assert(muse_sudoku_is_error(&st, empty));
    assert(muse_sudoku_is_error(&st, clue)); /* both ends flagged */
    muse_sudoku_clear(&st, empty);
    assert(st.grid[empty] == 0);
    assert(!muse_sudoku_is_error(&st, empty));
    assert(!muse_sudoku_is_error(&st, clue));
    assert(!muse_sudoku_is_complete(&st));

    /* Refusals: clues and out-of-range values. */
    assert(!muse_sudoku_set(&st, clue, 1));
    assert(st.grid[clue] == dup);
    assert(!muse_sudoku_set(&st, empty, 0));
    assert(!muse_sudoku_set(&st, empty, 7));
    assert(st.grid[empty] == 0);
    muse_sudoku_clear(&st, clue); /* no-op on a clue */
    assert(st.grid[clue] == dup);

    /* Hint: fills with the solution, then refuses (already correct). */
    assert(muse_sudoku_hint(&st, empty));
    assert(st.grid[empty] == muse_sudoku_solution(&st, empty));
    assert(!muse_sudoku_hint(&st, empty));
    assert(!muse_sudoku_hint(&st, clue));

    /* Index normalisation. */
    muse_sudoku_load(&st, 20);
    assert(st.idx == 0);
    muse_sudoku_load(&st, -1);
    assert(st.idx == 19);

    printf("ok\n");
    return 0;
}

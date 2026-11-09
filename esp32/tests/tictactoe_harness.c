/* Host harness for OX棋 (tic-tac-toe, mockup v4, 2026-11-08).
 * Builds with plain cc on the host (same pattern as pet_economy_harness.c):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I components/muse tests/tictactoe_harness.c \
 *      components/muse/muse_tictactoe.c -o /tmp/tictactoe
 * Covers the pure logic only (muse_tictactoe.c has no LVGL and no
 * timing -- the UI owns the 500 ms AI beat, so the harness drives the
 * AI by calling muse_tictactoe_ai_move() directly):
 *  1. Win detection on all 8 lines, for X and for O, plus no-winner.
 *  2. Draw detection: a full board with no line, and a played last move.
 *  3. AI takes an immediate win.
 *  4. AI usually blocks the player's immediate win (statistical: 400
 *     trials at BLOCK_PCT 75, wide bounds so no libc's rand() fails it)
 *     and sometimes does NOT block (the player's opening).
 *  5. A full game always terminates (random play, 20 games).
 *  6. Payout mapping: win 20 / draw 10 / loss 0 (the RPS model).
 *  7. Illegal moves refused, pause/resume/restart phases.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_tictactoe.h"

/* The 8 lines, same order as the logic's table (rows, columns,
 * diagonals) -- the harness derives its expectations from them. */
static const int k_lines[8][3] = {
    {0, 1, 2}, {3, 4, 5}, {6, 7, 8},
    {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
    {0, 4, 8}, {2, 4, 6},
};

/* A playing game with an arbitrary board (cells written directly:
 * the logic's struct is plain data, and winner()/win_line() are pure
 * functions of the board). */
static void set_board(muse_tictactoe_t *g, const int cells[9])
{
    muse_tictactoe_init(g);
    g->phase = MUSE_TICTACTOE_PLAYING;
    for (int i = 0; i < 9; i++) {
        g->cells[i] = (muse_tictactoe_mark_t)cells[i];
    }
}

/* --- 1. win detection ------------------------------------------------ */
static void test_win_all_lines(void)
{
    for (int line = 0; line < 8; line++) {
        int cells[9] = {0};
        for (int j = 0; j < 3; j++) {
            cells[k_lines[line][j]] = MUSE_TICTACTOE_X;
        }
        muse_tictactoe_t g;
        set_board(&g, cells);
        assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_X);
        assert(muse_tictactoe_win_line(&g) == line);
        int first = -1, last = -1;
        muse_tictactoe_line_cells(line, &first, &last);
        assert(first == k_lines[line][0]);
        assert(last == k_lines[line][2]);
        /* Same line held by O. */
        for (int j = 0; j < 3; j++) {
            cells[k_lines[line][j]] = MUSE_TICTACTOE_O;
        }
        set_board(&g, cells);
        assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_O);
        assert(muse_tictactoe_win_line(&g) == line);
    }
    /* No line: X at 0,1 and O at 3,4 -- nothing held. */
    int cells[9] = {0};
    cells[0] = MUSE_TICTACTOE_X;
    cells[1] = MUSE_TICTACTOE_X;
    cells[3] = MUSE_TICTACTOE_O;
    cells[4] = MUSE_TICTACTOE_O;
    muse_tictactoe_t g;
    set_board(&g, cells);
    assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_EMPTY);
    assert(muse_tictactoe_win_line(&g) == -1);
    assert(!muse_tictactoe_board_full(&g));
    int bad_first = 0, bad_last = 0;
    muse_tictactoe_line_cells(-1, &bad_first, &bad_last);
    assert(bad_first == -1 && bad_last == -1);
    muse_tictactoe_line_cells(8, &bad_first, &bad_last);
    assert(bad_first == -1 && bad_last == -1);
}

/* --- 2. draw detection ----------------------------------------------- */
static void test_draw(void)
{
    /* Classic draw, full board, no line:
     *   X O X
     *   X O O
     *   O X X */
    int cells[9] = {
        MUSE_TICTACTOE_X, MUSE_TICTACTOE_O, MUSE_TICTACTOE_X,
        MUSE_TICTACTOE_X, MUSE_TICTACTOE_O, MUSE_TICTACTOE_O,
        MUSE_TICTACTOE_O, MUSE_TICTACTOE_X, MUSE_TICTACTOE_X,
    };
    muse_tictactoe_t g;
    set_board(&g, cells);
    assert(muse_tictactoe_board_full(&g));
    assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_EMPTY);
    /* The same board reached by play: one empty cell left, X fills it. */
    cells[8] = MUSE_TICTACTOE_EMPTY;
    set_board(&g, cells);
    g.ai_turn = false;
    assert(muse_tictactoe_play(&g, 8));
    assert(g.phase == MUSE_TICTACTOE_OVER);
    assert(g.outcome == MUSE_TICTACTOE_DRAW);
    assert(g.win_line == -1);
}

/* --- played wins ------------------------------------------------------ */
static void test_play_wins(void)
{
    /* X holds 0,1; playing 2 takes the top row. */
    int cells[9] = {0};
    cells[0] = MUSE_TICTACTOE_X;
    cells[1] = MUSE_TICTACTOE_X;
    cells[3] = MUSE_TICTACTOE_O;
    cells[4] = MUSE_TICTACTOE_O;
    muse_tictactoe_t g;
    set_board(&g, cells);
    g.ai_turn = false;
    assert(muse_tictactoe_play(&g, 2));
    assert(g.phase == MUSE_TICTACTOE_OVER);
    assert(g.outcome == MUSE_TICTACTOE_WIN);
    assert(g.win_line == 0);
    /* A move after OVER is refused. */
    assert(!muse_tictactoe_play(&g, 5));
}

/* --- 3. AI takes an immediate win ------------------------------------- */
static void test_ai_takes_win(void)
{
    /* O holds 0,1; the AI must take 2 and win. */
    int cells[9] = {0};
    cells[0] = MUSE_TICTACTOE_O;
    cells[1] = MUSE_TICTACTOE_O;
    cells[3] = MUSE_TICTACTOE_X;
    cells[4] = MUSE_TICTACTOE_X;
    muse_tictactoe_t g;
    set_board(&g, cells);
    g.ai_turn = true;
    assert(muse_tictactoe_ai_move(&g) == 2);
    assert(g.phase == MUSE_TICTACTOE_OVER);
    assert(g.outcome == MUSE_TICTACTOE_LOSS);
    assert(g.win_line == 0);
}

/* --- 4. AI usually blocks (and sometimes does not) -------------------- */
static void test_ai_blocks_usually(void)
{
    /* X threatens the top row (0,1); O has no win of its own, so the
     * only correct move is the block at 2. A random fallback could
     * also land on 2 (1 in 6 empty cells), so the expected block rate
     * is ~0.75 + 0.25/6 = ~0.79. Wide bounds: any libc's rand() lands
     * inside 0.60..0.95 with overwhelming probability at n=400. */
    int cells[9] = {0};
    cells[0] = MUSE_TICTACTOE_X;
    cells[1] = MUSE_TICTACTOE_X;
    cells[3] = MUSE_TICTACTOE_O;
    int blocks = 0;
    int trials = 400;
    for (int t = 0; t < trials; t++) {
        muse_tictactoe_t g;
        set_board(&g, cells);
        g.ai_turn = true;
        int played = muse_tictactoe_ai_move(&g);
        assert(played >= 0 && played < 9);
        assert(g.cells[played] == MUSE_TICTACTOE_O);
        if (played == 2) {
            blocks++;
        }
    }
    double rate = (double)blocks / trials;
    assert(rate > 0.60 && rate < 0.95);
    assert(blocks < trials); /* sometimes it misses: the opening */
}

/* --- 5. full games terminate ------------------------------------------ */
static void test_full_games_terminate(void)
{
    for (int game = 0; game < 20; game++) {
        muse_tictactoe_t g;
        muse_tictactoe_init(&g);
        muse_tictactoe_start(&g);
        int plies = 0;
        while (g.phase == MUSE_TICTACTOE_PLAYING && plies < 9) {
            /* Player: a random empty cell. */
            int empties[9];
            int n = 0;
            for (int i = 0; i < 9; i++) {
                if (g.cells[i] == MUSE_TICTACTOE_EMPTY) {
                    empties[n++] = i;
                }
            }
            assert(n > 0);
            assert(muse_tictactoe_play(&g, empties[rand() % n]));
            plies++;
            if (g.phase == MUSE_TICTACTOE_PLAYING) {
                assert(g.ai_turn);
                int played = muse_tictactoe_ai_move(&g);
                assert(played >= 0 && played < 9);
                assert(g.cells[played] == MUSE_TICTACTOE_O);
            }
        }
        assert(g.phase == MUSE_TICTACTOE_OVER);
        assert(g.outcome != MUSE_TICTACTOE_NONE);
        /* The outcome agrees with the board. */
        if (g.outcome == MUSE_TICTACTOE_WIN) {
            assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_X);
            assert(g.win_line >= 0);
        } else if (g.outcome == MUSE_TICTACTOE_LOSS) {
            assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_O);
            assert(g.win_line >= 0);
        } else {
            assert(g.outcome == MUSE_TICTACTOE_DRAW);
            assert(muse_tictactoe_board_full(&g));
            assert(muse_tictactoe_winner(&g) == MUSE_TICTACTOE_EMPTY);
        }
    }
}

/* --- 6. payout mapping ------------------------------------------------- */
static void test_payout(void)
{
    assert(muse_tictactoe_payout_coins(MUSE_TICTACTOE_WIN) == 20u);
    assert(muse_tictactoe_payout_coins(MUSE_TICTACTOE_DRAW) == 10u);
    assert(muse_tictactoe_payout_coins(MUSE_TICTACTOE_LOSS) == 0u);
    assert(muse_tictactoe_payout_coins(MUSE_TICTACTOE_NONE) == 0u);
    /* The constants say the same, so a retune cannot desync the two. */
    assert(MUSE_TICTACTOE_PAYOUT_WIN == 20u);
    assert(MUSE_TICTACTOE_PAYOUT_DRAW == 10u);
    assert(MUSE_TICTACTOE_PAYOUT_LOSS == 0u);
}

/* --- 7. illegal moves and phases --------------------------------------- */
static void test_illegal_and_phases(void)
{
    muse_tictactoe_t g;
    muse_tictactoe_init(&g);
    assert(g.phase == MUSE_TICTACTOE_START);
    /* No moves before start. */
    assert(!muse_tictactoe_play(&g, 4));
    assert(muse_tictactoe_ai_move(&g) == -1);
    muse_tictactoe_start(&g);
    assert(g.phase == MUSE_TICTACTOE_PLAYING);
    assert(muse_tictactoe_play(&g, 4));
    /* Occupied, out of range, and wrong turn are all refused. */
    assert(!muse_tictactoe_play(&g, 4));
    assert(!muse_tictactoe_play(&g, -1));
    assert(!muse_tictactoe_play(&g, 9));
    assert(!muse_tictactoe_play(&g, 0)); /* AI's turn */
    assert(g.cells[4] == MUSE_TICTACTOE_X);
    /* Pause freezes play; resume thaws it (the AI answer is still due). */
    muse_tictactoe_pause(&g);
    assert(g.phase == MUSE_TICTACTOE_PAUSED);
    assert(!muse_tictactoe_play(&g, 0));
    assert(muse_tictactoe_ai_move(&g) == -1);
    muse_tictactoe_resume(&g);
    assert(g.phase == MUSE_TICTACTOE_PLAYING);
    assert(g.ai_turn);
    assert(muse_tictactoe_ai_move(&g) >= 0);
    /* Restart wipes the board and the turn. */
    muse_tictactoe_restart(&g);
    assert(g.phase == MUSE_TICTACTOE_PLAYING);
    assert(!g.ai_turn);
    for (int i = 0; i < 9; i++) {
        assert(g.cells[i] == MUSE_TICTACTOE_EMPTY);
    }
}

int main(void)
{
    /* Fixed seed: the block-rate test is statistical, and the wide
     * bounds hold for any libc, but a fixed seed keeps the run
     * reproducible on this machine. */
    srand(12345);
    test_win_all_lines();
    test_draw();
    test_play_wins();
    test_ai_takes_win();
    test_ai_blocks_usually();
    test_full_games_terminate();
    test_payout();
    test_illegal_and_phases();
    printf("ok\n");
    return 0;
}

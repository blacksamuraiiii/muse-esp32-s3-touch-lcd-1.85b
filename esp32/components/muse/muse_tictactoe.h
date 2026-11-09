/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tictactoe.h -- OX棋 logic (no LVGL, no ESP-IDF).
 *
 * A 3x3 board on the 240x240 field at (60,60): cells are 80 px, well
 * above the 60-70 px finger target, which is why tic-tac-toe fits this
 * 360 px round screen where 6x6 sudoku (~40 px cells) and 15x15 gomoku
 * (~16 px cells) do not (user decision 2026-11-08, mockup v4).
 *
 * The player is X and moves first; the AI is O. A round is one game:
 * the first side to take any of the 8 lines wins, a full board with
 * no line is a draw. The UI charges the 10-coin entry fee at 开始
 * through PET_FEE and settles once at OVER through PET_PLAY_REWARD,
 * paying back by outcome (win 20 / draw 10 / loss 0 -- the RPS
 * model, user decision 2026-11-08).
 *
 * The AI is deliberately easy (user decision: beatable by a 7-year-old):
 * it takes an immediate winning move when it has one, blocks the
 * player's immediate win MUSE_TICTACTOE_BLOCK_PCT percent of the time,
 * and otherwise plays a random empty cell. A missed block is the
 * player's opening: the AI never plays perfectly on purpose.
 *
 * Timing is nowhere in here: the UI owns the 500 ms delay before the
 * AI answers a move, so the host test drives the game deterministically
 * (no lv_tick_get() here). Random picks use stdlib rand(), seeded by
 * the caller (the UI seeds once from lv_tick_get on the first start,
 * as the snake page does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Field geometry, the same 240x240 square the other games use. */
#define MUSE_TICTACTOE_FIELD_X 60
#define MUSE_TICTACTOE_FIELD_Y 60
#define MUSE_TICTACTOE_FIELD_SIZE 240
#define MUSE_TICTACTOE_CELL 80
#define MUSE_TICTACTOE_CELLS 9

/* AI: how often it blocks the player's immediate win, 0..100. The
 * other 25% it plays randomly instead, which is the player's opening
 * -- a 7-year-old must be able to win (user decision 2026-11-08). */
#define MUSE_TICTACTOE_BLOCK_PCT 75

/* Payouts (coins) by outcome, the RPS model: the 10-coin entry fee
 * was already charged at start, so these are the gross paybacks the
 * UI sends once as PET_PLAY_REWARD (win nets +10, draw 0, loss -10). */
#define MUSE_TICTACTOE_PAYOUT_WIN 20u
#define MUSE_TICTACTOE_PAYOUT_DRAW 10u
#define MUSE_TICTACTOE_PAYOUT_LOSS 0u

typedef enum {
    MUSE_TICTACTOE_START = 0,
    MUSE_TICTACTOE_PLAYING,
    MUSE_TICTACTOE_PAUSED,
    MUSE_TICTACTOE_OVER,
} muse_tictactoe_phase_t;

typedef enum {
    MUSE_TICTACTOE_NONE = 0, /* game not over */
    MUSE_TICTACTOE_WIN,      /* player (X) took a line */
    MUSE_TICTACTOE_LOSS,     /* AI (O) took a line */
    MUSE_TICTACTOE_DRAW,     /* board full, no line */
} muse_tictactoe_outcome_t;

typedef enum {
    MUSE_TICTACTOE_EMPTY = 0,
    MUSE_TICTACTOE_X, /* player */
    MUSE_TICTACTOE_O, /* AI */
} muse_tictactoe_mark_t;

typedef struct {
    muse_tictactoe_phase_t phase;
    muse_tictactoe_mark_t cells[MUSE_TICTACTOE_CELLS];
    muse_tictactoe_outcome_t outcome; /* NONE until OVER */
    int win_line;                     /* 0..7 when a line won, else -1 */
    bool ai_turn;                     /* true once the player has moved
                                       * and the AI answer is pending */
} muse_tictactoe_t;

void muse_tictactoe_init(muse_tictactoe_t *g);

/* START -> PLAYING with an empty board, player (X) to move. No-op
 * from any other phase (use restart for a new game after OVER/PAUSED). */
void muse_tictactoe_start(muse_tictactoe_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_tictactoe_pause(muse_tictactoe_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_tictactoe_resume(muse_tictactoe_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_tictactoe_restart(muse_tictactoe_t *g);

/* Place the player's X on `cell` (0..8, row-major). False when not
 * PLAYING, when it is the AI's turn, when the cell is out of range
 * or already taken -- a refused move never changes the board. On a
 * legal move the game ends at once when the move wins or fills the
 * board (phase OVER, outcome + win_line set); otherwise ai_turn is
 * set and the UI answers with muse_tictactoe_ai_move(). */
bool muse_tictactoe_play(muse_tictactoe_t *g, int cell);

/* Play the AI's O (only when PLAYING and ai_turn is set). Returns the
 * cell played, or -1 when there is no move to make. Ends the game at
 * once on a win or a full board. */
int muse_tictactoe_ai_move(muse_tictactoe_t *g);

/* Who holds a line right now: EMPTY, X or O. */
int muse_tictactoe_winner(const muse_tictactoe_t *g);

/* The winning line's index (0..7) when a line is held, else -1. Line
 * order is rows, columns, diagonals; the UI draws the gold line
 * through the line's first and last cells. */
int muse_tictactoe_win_line(const muse_tictactoe_t *g);

/* The line's first and last cells (row-major 0..8) for `line` (0..7).
 * The UI draws the gold win line through these two cells' centres. */
void muse_tictactoe_line_cells(int line, int *first, int *last);

/* True when every cell is taken. Only meaningful while PLAYING: the
 * game ends as a draw the moment the board fills with no winner. */
bool muse_tictactoe_board_full(const muse_tictactoe_t *g);

/* Outcome -> gross payout coins (win 20 / draw 10 / loss 0). NONE
 * pays 0: the UI only calls this at OVER. */
uint32_t muse_tictactoe_payout_coins(muse_tictactoe_outcome_t outcome);

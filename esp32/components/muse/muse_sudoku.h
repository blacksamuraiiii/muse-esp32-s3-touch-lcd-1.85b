/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_sudoku.h -- 6x6 Sudoku game logic (no LVGL, no ESP-IDF).
 *
 * Pure logic on purpose: the UI (muse_sudoku_ui.c) owns all LVGL objects,
 * this file owns only the board. It compiles on the host with plain cc,
 * so tests/test_sudoku_logic.py can verify every rule without a board.
 *
 * Board: 6 rows x 6 cols, values 1..6, 0 = empty. Boxes are 2 rows x
 * 3 cols (box row = r / 2, box col = c / 3). Positions are row-major
 * indices 0..35 (pos = r * 6 + c).
 *
 * Errors: a filled cell is an error when its value appears at another
 * cell in the same row, column or box. All cells in a duplicate set are
 * flagged, clues included -- the UI colours them red so the player sees
 * both ends of the conflict.
 *
 * v1 scope (user decision 2026-10-31, mockups gameplay-v3/popup-v3):
 * built-in puzzles only, no on-device generation; a wrong entry is only
 * flagged red, never rejected and never ends the game (no 3-strike rule
 * yet); no draft/pencil marks; progress lives in RAM (a reboot restarts
 * the current puzzle, re-entering the page resumes it).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MUSE_SUDOKU_SIZE 6
#define MUSE_SUDOKU_CELLS 36
#define MUSE_SUDOKU_COUNT 20

typedef struct {
    int idx;                          /* 0..19, which puzzle is loaded */
    uint8_t grid[MUSE_SUDOKU_CELLS];  /* current values, 0 = empty */
    uint8_t given[MUSE_SUDOKU_CELLS]; /* 1 = clue, never editable */
    uint8_t err[MUSE_SUDOKU_CELLS];   /* 1 = value duplicates row/col/box */
} muse_sudoku_state_t;

/* Number of built-in puzzles (20). */
int muse_sudoku_count(void);

/* Load puzzle idx (normalised modulo the count, so 20 wraps to 0 and
 * -1 to 19). Resets grid to the givens, clears all errors. */
void muse_sudoku_load(muse_sudoku_state_t *st, int idx);

/* 1 when pos is a clue (not editable), 0 otherwise. */
bool muse_sudoku_is_given(const muse_sudoku_state_t *st, int pos);

/* Current value at pos (0 = empty). */
uint8_t muse_sudoku_value(const muse_sudoku_state_t *st, int pos);

/* 1 when pos is currently flagged as a conflict, 0 otherwise. */
bool muse_sudoku_is_error(const muse_sudoku_state_t *st, int pos);

/* Solution value at pos (the puzzle's verified unique solution). */
uint8_t muse_sudoku_solution(const muse_sudoku_state_t *st, int pos);

/* Set a user value (1..6) at pos. False (no change) when pos is a clue
 * or val is out of range; otherwise sets the cell and recomputes all
 * error flags. A conflicting value is still stored, only flagged. */
bool muse_sudoku_set(muse_sudoku_state_t *st, int pos, uint8_t val);

/* Clear a user entry at pos. No-op on clues and on already-empty cells.
 * Recomputes error flags. */
void muse_sudoku_clear(muse_sudoku_state_t *st, int pos);

/* Fill pos with its solution value. False (no change) when pos is a clue
 * or already holds the solution value; otherwise fills it and recomputes
 * error flags. The UI picks pos: the selected cell when it is fillable,
 * else the first empty cell (see muse_sudoku_first_empty). */
bool muse_sudoku_hint(muse_sudoku_state_t *st, int pos);

/* First empty cell (0..35), or -1 when the board is full. */
int muse_sudoku_first_empty(const muse_sudoku_state_t *st);

/* True when every cell is filled and no error flag is set. */
bool muse_sudoku_is_complete(const muse_sudoku_state_t *st);

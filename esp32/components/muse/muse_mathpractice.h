/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_mathpractice.h -- 数学练习 logic (no LVGL, no ESP-IDF).
 *
 * One 2-minute round of two question kinds (user decision 2026-11-08,
 * the round that fused 凑十法练习 into this game):
 *
 *   - ADD_TEN: a + b with a, b in 2..9 and 11 <= a + b <= 18, so the
 *     make-ten split always applies. The screen reads "8 + 5 ? 13".
 *   - SUB_TEN: a - b with the minuend a in 11..18 and b in
 *     (a % 10 + 1)..9, so the ones digit is smaller than b and the
 *     subtraction must borrow ten: "14 - 6 ? 8".
 *
 * The two kinds are dealt by a coin flip, so a round averages 1:1.
 * The comparison number d is the true value or a near-miss of it (off
 * by 1 or 2, never equal), so the player has to compute and then
 * compare; the player taps > < or =, and the statement is "left ? d".
 *
 * A pick is graded at once: right is +1, wrong is +0 (never
 * negative). The verdict stays on screen for
 * MUSE_MATHPRACTICE_FEEDBACK_MS (user 2026-11-08: 点击之后要提示正确
 * 还是错误), and during that window nothing is dealt and no answer is
 * taken -- a fast double tap can never skip a question. The round
 * clock keeps running through the verdict (it is round time).
 *
 * Score only ever goes up, so the settlement is
 * muse_pet_round_coins_cap() with this game's own 20-coin ceiling (1
 * point per coin; raised from the shared 10 on 2026-11-08 because the
 * round is 3 minutes now). The 10-coin entry fee was already charged at
 * start.
 *
 * Timing is in milliseconds and advances only through
 * muse_mathpractice_tick(), so the host test drives it
 * deterministically (no lv_tick_get() here). The UI calls tick every
 * 50 ms and pauses its timer while paused, so tick() also no-ops
 * unless PLAYING. Random picks use stdlib rand(), seeded by the
 * caller (the UI seeds once from lv_tick_get on the first start, as
 * the snake page does for food placement).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One round is 2 minutes (user 2026-11-09, was 3 minutes). */
#define MUSE_MATHPRACTICE_GAME_MS 120000u
#define MUSE_MATHPRACTICE_TICK_MS 50u
/* How long the verdict stays up before the next question is dealt
 * (user 2026-11-08). Answer taps are ignored during it. */
#define MUSE_MATHPRACTICE_FEEDBACK_MS 600u
/* Operands, values and the comparison number all live in 0..20. */
#define MUSE_MATHPRACTICE_MAX_NUM 20
/* ADD_TEN: addends are single digits 2..9, the sum 11..18 (a carry,
 * so 凑十法 applies). SUB_TEN: the minuend is 11..18. */
#define MUSE_MATHPRACTICE_ADDEND_MIN 2
#define MUSE_MATHPRACTICE_ADDEND_MAX 9
#define MUSE_MATHPRACTICE_SUM_MIN 11
#define MUSE_MATHPRACTICE_SUM_MAX 18
#define MUSE_MATHPRACTICE_MINUEND_MIN 11
#define MUSE_MATHPRACTICE_MINUEND_MAX 18

typedef enum {
    MUSE_MATHPRACTICE_START = 0,
    MUSE_MATHPRACTICE_PLAYING,
    MUSE_MATHPRACTICE_PAUSED,
    MUSE_MATHPRACTICE_OVER,
} muse_mathpractice_phase_t;

typedef enum {
    MUSE_MATHPRACTICE_KIND_ADD_TEN = 0, /* 个位数相加：a + b（凑十法） */
    MUSE_MATHPRACTICE_KIND_SUB_TEN,     /* 十几减个位：a - b（借十法） */
} muse_mathpractice_kind_t;

typedef enum {
    MUSE_MATHPRACTICE_CMP_GT = 0, /* 等式的值 > 比较数 */
    MUSE_MATHPRACTICE_CMP_LT,     /* 等式的值 < 比较数 */
    MUSE_MATHPRACTICE_CMP_EQ,     /* 等式的值 == 比较数 */
} muse_mathpractice_cmp_t;

typedef struct {
    muse_mathpractice_phase_t phase;
    muse_mathpractice_kind_t kind;
    int a; /* ADD_TEN: first addend 2..9; SUB_TEN: minuend 11..18 */
    int b; /* ADD_TEN: second addend 2..9; SUB_TEN: subtrahend 2..9 */
    int d; /* the number "?" compares against, 0..20 */
    muse_mathpractice_cmp_t correct; /* the comparison that is true */
    uint32_t feedback_ms; /* > 0: verdict showing, no new question */
    bool last_right;      /* how the last pick was graded (for the UI) */
    uint32_t score;       /* +1 per right pick, never decreases */
    uint32_t answered;    /* picks graded so far */
    uint32_t elapsed_ms;
} muse_mathpractice_t;

void muse_mathpractice_init(muse_mathpractice_t *g);

/* START -> PLAYING, resetting the game and dealing the first
 * question. No-op from any other phase (use restart for a new game
 * after OVER/PAUSED). */
void muse_mathpractice_start(muse_mathpractice_t *g);

/* PLAYING -> PAUSED. No-op from any other phase. */
void muse_mathpractice_pause(muse_mathpractice_t *g);

/* PAUSED -> PLAYING. No-op from any other phase; never re-charges. */
void muse_mathpractice_resume(muse_mathpractice_t *g);

/* Any phase -> PLAYING with a fresh game (a new game, so the UI
 * charges the entry fee again before calling this). */
void muse_mathpractice_restart(muse_mathpractice_t *g);

/* Advance the game by `ms` milliseconds. No-op unless PLAYING. Runs
 * the 2-minute clock and the verdict window: when the verdict time is
 * up the next question is dealt. Ends the game at 2 minutes (OVER),
 * which also clears a pending verdict. */
void muse_mathpractice_tick(muse_mathpractice_t *g, uint32_t ms);

/* Pick a comparison: `cmp` is the button the player tapped (GT for
 * >, LT for <, EQ for =). Returns true when the pick was right
 * (score +1), false otherwise (score unchanged). Either way the
 * verdict shows for MUSE_MATHPRACTICE_FEEDBACK_MS and the next
 * question appears when it ends. No-op (returns false, nothing
 * changes) unless PLAYING and no verdict is showing. */
bool muse_mathpractice_answer(muse_mathpractice_t *g, muse_mathpractice_cmp_t cmp);

/* The true value of the current equation (a + b or a - b). */
int muse_mathpractice_value(const muse_mathpractice_t *g);

/* Remaining whole seconds, rounded up (120 at start, 0 at the end). */
uint32_t muse_mathpractice_remaining_s(const muse_mathpractice_t *g);

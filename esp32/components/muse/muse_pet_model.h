/* Public-edition staging: local UI additions and/or privacy review; see source manifest. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet_model.h -- 1.5a gold-only pet economy model.
 *
 * Adapted from TamaMe (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet_model.h, Apache-2.0).
 * TamaMe attribution is retained above; the economy below is NOT TamaMe's:
 * hunger / mood / energy, hourly decay, 36-second quanta and the food table
 * are all deleted (design 20261025-1.5互动预览版设计方案.md §4, user decision
 * 2026-10-25: "不要饱腹、全部用金币替代").
 *
 * Differences from TamaMe, deliberate and documented in PHASE1-NOTES.md:
 *  - MUSE_PET_VERSION is 2, not 1: the struct layout is incompatible with
 *    TamaMe v1, so a TamaMe blob must fail validation (and size check) rather
 *    than be misread as a 1.5a state.
 *  - No muse_i18n.h dependency: fortune cards, grades and food names live in
 *    muse_pet_text.h/.c as static Chinese tables.
 *  - No hunger / mood / energy / sfx / last_time fields at all (design §4.1
 *    allows keeping them inert, but dropping them is stronger: they cannot
 *    silently affect coins).
 *  - Feed is three action values (rice / cake / coffee) that settle
 *    identically. The design calls feed one action; the three UI buttons need
 *    a way for phase-2 UI to know which food was fed for its subtitle
 *    ("喂了<食物名>"), and the command queue carries only an action value,
 *    so the food is encoded in the action. Reward is identical for all three.
 *  - Day numbers come from muse_pet_day(), which is a UTC day
 *    (epoch / 86400 + MUSE_PET_DAY_OFFSET_SECONDS, TZ-independent,
 *    user decision 2026-10-25). The boundary is 08:00 Beijing time
 *    with the default offset 0, not local midnight.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Struct layout version. Bump this whenever the layout below changes. */
#define MUSE_PET_VERSION 4u

/* Economy constants (design §4). All amounts are whole coins. */
#define MUSE_PET_COINS_MAX 100u        /* wallet cap: excess is discarded */
#define MUSE_PET_TOUCH_COINS 5u        /* per touch */
#define MUSE_PET_TOUCH_DAILY_MAX 50u   /* per day: the first 10 touches pay */
#define MUSE_PET_FEED_COINS 10u        /* per food, once per food per day */
#define MUSE_PET_FEED_KINDS 6u         /* rice / cake / coffee / burger / pizza / milk tea */
#define MUSE_PET_FEED_DAILY_MAX 60u    /* six foods x 10 (user decision 2026-11-07) */
#define MUSE_PET_GAME_STAKE 5u         /* win +5, lose -5, draw 0; also the entry fee
                                        * of every mini-game (PET_FEE / PET_SUDOKU_ENTRY) */
#define MUSE_PET_FORTUNE_COINS 20u     /* once a day, paid with the new card */
#define MUSE_PET_DECAY_PER_DAY 20u     /* living cost, per calendar day */
#define MUSE_PET_FORTUNES 20u          /* fortune cards in muse_pet_text.c */

/*
 * Score -> coin conversion (user decision 2026-11-01, corrected the same
 * day; retuned 2026-11-01 again after 1.5f shipped the 5-coin ceiling as
 * "v4": the two score games follow the rock-paper-scissors rule -- a
 * round costs the 5-coin entry fee up front (PET_FEE) and pays back
 * 1 coin per point, so the round's NET is payout minus fee:
 *
 *   payout = min(MUSE_PET_ROUND_COIN_MAX, score) = min(10, score)
 *   net    = payout - 5     ->  10 points = +5, 5 points = 0 (a draw),
 *                               0 points = -5 (a loss), exactly the
 *                               win / draw / loss of rock-paper-scissors.
 *
 * The conversion lives in muse_pet_round_coins() and nowhere else, so
 * snake and whack-a-mole cannot drift apart. The 10-coin ceiling is what
 * keeps games from minting coins -- not the curve, which is 1:1.
 */
#define MUSE_PET_SCORE_PER_COIN 1u     /* points per coin (1:1) */
#define MUSE_PET_ROUND_COIN_MAX 10u    /* per-round payout ceiling */

/*
 * Sudoku pays in completions, not points: the first puzzle solved on a
 * calendar day pays MUSE_PET_SUDOKU_SOLVE_COINS (10), and no later puzzle
 * that day pays anything -- the daily grant is all-or-nothing, so a
 * sudoku_reward_coins that is nonzero for any reason already means "paid
 * today". That includes a blob written by 1.5f's v4, which stored 5 for
 * its first solve: under this rule it counts as paid and is NOT topped
 * up to 10 (user decision 2026-11-01, transition edge). The entry fee
 * itself is charged ONCE per day (PET_SUDOKU_ENTRY), no matter how many
 * puzzles the player flips through afterwards, so a sudoku day nets at
 * most +5 (ticket 5, first win 10).
 */
#define MUSE_PET_SUDOKU_SOLVE_COINS 10u /* the day's first completed puzzle */
#define MUSE_PET_SUDOKU_DAILY_MAX 10u   /* per day: the first completion pays */

/*
 * "No real time yet" threshold, taken from muse_rtc.c ("Older than this means
 * 'no real time yet': 2025-01-01 00:00:00 UTC"). A clock below this is
 * treated as uncalibrated: no decay, no daily-cap reset (design §4.2).
 */
#define MUSE_PET_TIME_MIN 1735689600LL /* 2025-01-01 00:00:00 UTC */

/* State, persisted as one NVS blob (namespace "pet_pub_stats", key "state").
 * Day fields are calendar-day numbers from muse_pet_day(), or -1 when unset.
 * fortune is meaningful only when fortune_day >= 0.
 * decay_notice is the living-cost amount still to be announced once; phase-2
 * UI clears it with PET_DECAY_ACK after showing it (design §4.2).
 * The sudoku_* trio is the mini-game daily ledger (v4, user decision
 * 2026-11-01; grant retuned 2026-11-01, layout unchanged so the version
 * stays 4): sudoku_entry_paid is 1 once that day's 5-coin entry fee has
 * been settled, and sudoku_reward_coins is the coins this day's solved
 * puzzles have paid (0, or 10 once the day's first completion has paid;
 * a v4 blob's stored 5 also counts as paid, never topped up). Both roll
 * when sudoku_day advances; see muse_pet_sudoku_today(). */
typedef struct {
    uint32_t version;
    uint32_t coins;              /* 0..100 */
    int32_t touch_day;           /* day of the current touch cap, -1 = none */
    uint32_t daily_touch_coins;  /* 0..50 */
    int32_t feed_day;            /* day of the current feed cap, -1 = none */
    uint32_t feed_mask;          /* bits 0..2: that food already paid today */
    int32_t fortune_day;         /* day of the current fortune, -1 = never */
    uint32_t fortune;            /* 0..19, valid only if fortune_day >= 0 */
    int32_t decay_day;           /* last day living cost was settled, -1 = none */
    uint32_t decay_notice;       /* amount to announce once, 0 = nothing pending */
    int32_t sudoku_day;          /* day of the sudoku ledger, -1 = none */
    uint32_t sudoku_entry_paid;  /* 1 = today's sudoku entry fee is paid */
    uint32_t sudoku_reward_coins;/* 0..5, paid by today's solved puzzles */
} muse_pet_model_t;

/* Actions. PET_FEED_* settle identically; see the header note above.
 * PET_DECAY_ACK and PET_RESET are internal only: never sent by UI buttons.
 * PET_DECAY_ACK clears decay_notice after phase-2 UI has shown it once;
 * since 1.5b its arg is the announced decay_day, and only a notice for
 * that day is cleared (see muse_pet_decay_ack()).
 * PET_RESET rebuilds the initial state; only the Test-gated console command
 * may send it (see the muse_input.c patch note).
 * PET_FEE deducts one round's entry fee (MUSE_PET_GAME_STAKE) for a snake
 * or whack-a-mole game (user decision 2026-10-31): the UI gates on the
 * snapshot first, and the worker re-checks -- under 5 coins it returns
 * PET_POOR and settles nothing, exactly like PET_GAME's gate. It never
 * changes the persisted layout.
 * PET_SUDOKU_ENTRY is the sudoku page's entry fee (user decision 2026-11-01):
 * charged at most ONCE per calendar day, idempotent -- a second request on
 * the same day is PET_OK and moves nothing, so flipping puzzles (上一题 /
 * 下一题) and 再玩一次 are all free. Under 5 coins and unpaid it returns
 * PET_POOR, again without settling.
 * PET_SUDOKU_SOLVE is the completion reward (arg unused): it pays
 * MUSE_PET_SUDOKU_SOLVE_COINS (10) for the day's first completion and
 * PET_CAP for every later puzzle -- and for any day whose ledger already
 * holds a nonzero reward, including a v4 blob's 5, which is treated as
 * paid and never topped up to 10.
 * PET_PLAY_REWARD settles a finished score-based round: arg is the coin
 * amount the UI converted with muse_pet_round_coins(), clamped here to
 * 0..MUSE_PET_ROUND_COIN_MAX (10) so a UI bug cannot mint coins. The
 * round's net is that payout minus the 5-coin entry fee the UI already
 * charged with PET_FEE; the model never nets the two itself. */
typedef enum {
    PET_TOUCH = 0,
    PET_FEED_RICE,
    PET_FEED_CAKE,
    PET_FEED_COFFEE,
    PET_FEED_BURGER,
    PET_FEED_PIZZA,
    PET_FEED_MILKTEA,
    PET_GAME,
    PET_FORTUNE,
    PET_DECAY_ACK,
    PET_RESET,
    PET_FEE,
    PET_SUDOKU_ENTRY,
    PET_SUDOKU_SOLVE,
    PET_PLAY_REWARD,
} muse_pet_action_t;

/* Results. UI copy must be keyed off these, never guessed (design §4.3).
 * PET_OK for feed means "settled": whether coins were actually added is read
 * from the snapshot's feed_mask, not from a separate code (design §4.3).
 * Fortune never returns a "no day" refusal any more (phase 2, user
 * decision): it always uses the current UTC day. */
typedef enum {
    PET_OK = 0,
    PET_COIN,          /* touch paid */
    PET_CAP,           /* daily cap reached: touch = happy only, no coins;
                        * sudoku = today's 5 coins already paid, no coins */
    PET_POOR,          /* game/fee: coins < 5, not settled, no round started */
    PET_FORTUNE_NEW,   /* new card drawn */
    PET_FORTUNE_SAME,  /* same-day (or prior) card shown again */
    PET_STORAGE,       /* NVS write failed: state was NOT changed */
    PET_BUSY,          /* unknown action */
    /* PET_TIME removed 2026-10-25 (user decision): fortune no longer
     * refuses on an uncalibrated clock; it always uses the current UTC
     * day (see muse_pet_day()). Kept out of the enum on purpose so no
     * caller can still branch on it. */
} muse_pet_result_t;

void muse_pet_model_init(muse_pet_model_t *s);
bool muse_pet_model_valid(const muse_pet_model_t *s);

/* Coins a finished round's score converts to: min(10, score), i.e. 1:1 with
 * the 10-coin ceiling. The only place the conversion curve lives, so snake
 * and whack-a-mole cannot drift apart. Call it with the round's final score
 * at OVER, never per point. The entry fee (5, charged at start through
 * PET_FEE) is NOT subtracted here: the UI shows payout and net separately,
 * so the conversion must stay the gross payout. */
uint32_t muse_pet_round_coins(uint32_t score);

/* Day-boundary offset, in seconds, added to the UTC epoch before dividing
 * by 86400. 8*3600 puts the boundary at local midnight in China (UTC+8),
 * which is what the owner reads as "today" (user decision 2026-11-05: with
 * 0 the day rolled at 08:00 Beijing time, so a feed before 8 a.m. still hit
 * yesterday's ledger and answered "already fed today" every morning).
 * Change ONLY this constant to move the boundary; no call-site logic
 * changes. The day number stays TZ-independent in shape -- it is still
 * epoch arithmetic, just shifted -- so fortune / living cost / the daily
 * caps all roll together. */
#define MUSE_PET_DAY_OFFSET_SECONDS (8 * 3600)

/*
 * UTC calendar-day number for `now` (epoch / 86400, gmtime semantics),
 * plus MUSE_PET_DAY_OFFSET_SECONDS. Deliberately TZ-independent: the board
 * clock is UTC (muse_time_start() RTC adopt + SNTP), and fortune / living
 * cost are defined on that UTC day (user decision 2026-10-25, superseding
 * the phase-1 localtime_r() + TZ design). *calibrated is set from
 * MUSE_PET_TIME_MIN only (used by the living-cost gate and the daily-cap
 * roll, NOT by fortune any more: fortune always uses the current UTC day,
 * calibrated or not, so a later SNTP jump can legitimately draw one extra
 * card -- accepted preview behaviour, not a bug).
 */
int32_t muse_pet_day(int64_t now, bool *calibrated);

/*
 * Today's sudoku ledger, as a UI can read it without duplicating the day
 * rule: *entry_paid is true when the 5-coin entry fee has already been
 * settled today (so entering is free), and *reward_coins is how many coins
 * today's solved puzzles have paid (0, or 10 once the day's first
 * completion has paid -- a v4 blob's stored 5 reads back as 5, which the
 * UI must still treat as "already paid": only 0 means a completion will
 * pay). Both are derived from the snapshot; nothing is written. Either
 * pointer may be NULL.
 */
void muse_pet_sudoku_today(const muse_pet_model_t *s, int64_t now,
                           bool *entry_paid, uint32_t *reward_coins);

/* Settle one calendar day's living cost into `s` (idempotent for a given day).
 * No-op when uncalibrated, and never moves decay_day backwards on rollback. */
void muse_pet_model_decay(muse_pet_model_t *s, int64_t now);

/* Roll the daily caps (feed mask + touch count) forward when the calibrated
 * day has advanced, without settling any action. The worker tick calls this
 * so the snapshot's feed_mask / touch count are fresh within one tick of the
 * day boundary: without it the caps are only rolled when a feed/touch action
 * is applied, so on a new day the UI reads yesterday's mask until the first
 * feed/touch of that day (2026-11-07: rice always answered "already fed
 * today" because it is the first button). Idempotent and rollback-safe:
 * roll_cap() only moves forward, and an uncalibrated clock rolls nothing.
 * Fortune is deliberately NOT rolled here: it draws on demand (PET_FORTUNE
 * checks fortune_day at draw time), and the sudoku ledger is read through
 * muse_pet_sudoku_today(), which compares sudoku_day to today. */
void muse_pet_model_roll_daily(muse_pet_model_t *s, int64_t now);

/* Settle one action into `s` (decay is applied first). `random` is used only
 * for PET_FORTUNE (index = random % 20). `arg` carries the outcome of a game
 * round (-1 lose / 0 draw / +1 win, decided UI-side at click time, design
 * §5.2) and is ignored by every other action: since 1.5a a round costs 5 on a
 * loss, pays 5 on a win and costs nothing on a draw. */
muse_pet_result_t muse_pet_model_apply(muse_pet_model_t *s, muse_pet_action_t action,
                                       int64_t now, uint32_t random, int32_t arg);

/* True for the three feed actions. */
bool muse_pet_action_is_feed(muse_pet_action_t action);

/* 0..2 (rice / cake / coffee) for a feed action, -1 for anything else. The
 * index is the bit in feed_mask, so each food pays once per day on its own. */
int muse_pet_feed_index(muse_pet_action_t action);

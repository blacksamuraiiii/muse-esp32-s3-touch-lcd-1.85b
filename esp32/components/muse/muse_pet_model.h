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
 *  - Feed is six action values (rice / cake / coffee / hamburger / pizza /
 *    milk tea) that settle identically. The design calls feed one action;
 *    the six UI buttons need a way for phase-2 UI to know which food was fed
 *    for its subtitle ("喂了<食物名>"), and the command queue carries only an
 *    action value, so the food is encoded in the action. Reward is identical
 *    for all six.
 *  - Day numbers come from muse_pet_day(), which is a UTC day
 *    (epoch / 86400 + MUSE_PET_DAY_OFFSET_SECONDS, TZ-independent,
 *    user decision 2026-10-25). The boundary is local midnight
 *    (offset 8*3600, retuned 2026-11-05), not 08:00 Beijing time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The Test build compiles this header with the same sdkconfig the rest of
 * the firmware sees; the host harnesses have no sdkconfig at all, so the
 * include has to be optional (see MUSE_PET_JIAOBEI_DAILY_MAX). */
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/* Struct layout version. Bump this whenever the layout below changes. */
#define MUSE_PET_VERSION 8u

/* Economy constants (design §4). All amounts are whole coins. */
#define MUSE_PET_COINS_MAX 100u        /* wallet cap: excess is discarded */
#define MUSE_PET_TOUCH_COINS 5u        /* per touch */
#define MUSE_PET_TOUCH_DAILY_MAX 50u   /* per day: the first 10 touches pay */
#define MUSE_PET_FEED_COINS 10u        /* per food, once per food per day */
#define MUSE_PET_FEED_KINDS 6u         /* rice / cake / coffee / burger / pizza / milk tea */
#define MUSE_PET_FEED_DAILY_MAX 60u    /* six foods x 10 (user decision 2026-11-07) */
#define MUSE_PET_GAME_STAKE 10u        /* entry fee of every mini-game
                                        * (PET_FEE) and the rock-paper-
                                        * scissors round: win +10 net,
                                        * draw 0, loss -10 (user decision
                                        * 2026-11-08, retuned from 5) */
#define MUSE_PET_FORTUNE_COST 2u     /* one draw a day, charged (user
                                        * decision 2026-11-16: it was 5,
                                        * the draw used to pay 20 before
                                        * 2026-11-14) */
#define MUSE_PET_JIAOBEI_COST 2u     /* one 掷圣杯 toss (PET_JIAOBEI_TOSS;
                                        * 5 until the user set it to 2 on
                                        * 2026-11-16) */
/* Tosses per day. The Test build has no cap: the user is exercising the
 * board and hit the 9th toss within minutes (2026-11-16, "把今天的 10 次
 * 限制给取消，我要做测试"). Release keeps 9. The host harnesses build
 * without sdkconfig, so they always see the Release number and keep
 * testing the cap itself. */
#if defined(CONFIG_MUSE_BUILD_TEST) && CONFIG_MUSE_BUILD_TEST
#define MUSE_PET_JIAOBEI_DAILY_MAX 999999u /* effectively unlimited */
#else
#define MUSE_PET_JIAOBEI_DAILY_MAX 9u      /* tosses per day (user decision
                                            * 2026-11-14); the 10th is
                                            * PET_CAP */
#endif
/* Structural upper bound for the 掷圣杯 ledger (jiaobei_count /
 * jiaobei_streak), used by muse_pet_model_valid() only. It is NOT the
 * daily policy cap: that is MUSE_PET_JIAOBEI_DAILY_MAX and is still
 * enforced per toss by muse_pet_model_apply(). valid() only checks that
 * a stored blob is structurally sane, so it must not depend on a
 * build-time policy constant -- a Test build legitimately stores more
 * than 9 tosses in a day, and that blob must still load on Release
 * (2026-11-17, B2). */
#define MUSE_PET_JIAOBEI_LEDGER_MAX 999999u
#define MUSE_PET_LUCKYWHEEL_COST 2u     /* one lucky wheel spin
                                        * (PET_LUCKYWHEEL_SPIN; user
                                        * decision 2026-11-20) */
/* Spins per day. Same Test/Release split as 掷圣杯: the Test build has
 * no cap, Release keeps 9. The host harnesses build without sdkconfig,
 * so they always see the Release number and keep testing the cap. */
#if defined(CONFIG_MUSE_BUILD_TEST) && CONFIG_MUSE_BUILD_TEST
#define MUSE_PET_LUCKYWHEEL_DAILY_MAX 999999u /* effectively unlimited */
#else
#define MUSE_PET_LUCKYWHEEL_DAILY_MAX 9u      /* spins per day (user decision
                                            * 2026-11-20); the 10th is
                                            * PET_CAP */
#endif
/* Structural upper bound for the lucky wheel ledger
 * (luckywheel_count), used by muse_pet_model_valid() only -- same
 * reasoning as MUSE_PET_JIAOBEI_LEDGER_MAX (B2). */
#define MUSE_PET_LUCKYWHEEL_LEDGER_MAX 999999u
#define MUSE_PET_DECAY_PER_DAY 20u     /* living cost, per calendar day */
#define MUSE_PET_FORTUNES 20u          /* fortune cards in muse_pet_text.c */

/*
 * Score -> coin conversion (user decision 2026-11-01, corrected the same
 * day; retuned 2026-11-01 again after 1.5f shipped the 5-coin ceiling as
 * "v4": the two score games follow the rock-paper-scissors rule -- a
 * round costs the 10-coin entry fee up front (PET_FEE) and pays back
 * 1 coin per point, so the round's NET is payout minus fee:
 *
 *   payout = min(MUSE_PET_ROUND_COIN_MAX, score) = min(20, score)
 *   net    = payout - 10    ->  20 points = +10, 10 points = 0 (a draw),
 *                               0 points = -10 (a loss), exactly the
 *                               win / draw / loss of rock-paper-scissors
 *                               (retuned from 5 / 10 on 2026-11-08).
 *
 * The conversion lives in muse_pet_round_coins() (and its capped variant
 * muse_pet_round_coins_cap(), which the math game uses with its own
 * ceiling), so the five games cannot drift apart. The 20-coin ceiling is
 * what keeps games from minting coins -- not the curve, which is 1:1.
 */
#define MUSE_PET_SCORE_PER_COIN 1u     /* points per coin (1:1) */
#define MUSE_PET_ROUND_COIN_MAX 20u    /* per-round payout ceiling */
/* 数学练习 has its own 20-coin ceiling: raised from the shared 10 on
 * 2026-11-08 when the round went to 3 minutes, and KEPT at 20 when the
 * round came back to 2 minutes on 2026-11-09 (user decision: only the
 * clock changed). Every other game also uses 20
 * (MUSE_PET_ROUND_COIN_MAX, retuned 2026-11-08), so this constant is the
 * same value; it stays as its own knob in case this game ever wants a
 * different ceiling. The worker clamps PET_PLAY_REWARD to the hard
 * maximum below, so a UI bug still cannot mint more than any game's
 * ceiling. */
#define MUSE_PET_ROUND_COIN_HARD_MAX MUSE_PET_COINS_MAX /* the wallet: one
                                        * round can fill it and no more
                                        * (2026-11-17, was 20 -- the maths
                                        * page has no ceiling below the
                                        * wallet any more) */

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
 * most +5 (ticket 5, first win 10). The entry fee is its own constant
 * (MUSE_PET_SUDOKU_ENTRY_COINS, 5): sudoku does NOT follow the 2026-11-08
 * economy retune (user decision, "数独不跟" -- it is archived for now
 * because its 6x6 grid is too small to read on the 360px round screen),
 * so it stays 5 even though the other games' entry fee is now 10.
 */
#define MUSE_PET_SUDOKU_ENTRY_COINS 5u  /* sudoku's daily ticket, decoupled
                                        * from MUSE_PET_GAME_STAKE */
#define MUSE_PET_SUDOKU_SOLVE_COINS 10u /* the day's first completed puzzle */
#define MUSE_PET_SUDOKU_DAILY_MAX 10u   /* per day: the first completion pays */
#define MUSE_PET_TRACING_COINS 2u       /* one traced character, once per
                                         * character per day (user decision
                                         * 2026-11-20, same as math practice
                                         * per question) */
#define MUSE_PET_TRACING_CHARS 10u      /* 天 地 人 日 月 山 川 木 水 火 */

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
 * stays 4): sudoku_entry_paid is 1 once that day's 5-coin sudoku entry
 * fee (MUSE_PET_SUDOKU_ENTRY_COINS) has been settled, and
 * sudoku_reward_coins is the coins this day's solved puzzles have paid
 * (0, or 10 once the day's first completion has paid; a v4 blob's stored
 * 5 also counts as paid, never topped up). Both roll when sudoku_day
 * advances; see muse_pet_sudoku_today(). */
typedef struct {
    uint32_t version;
    uint32_t coins;              /* 0..100 */
    int32_t touch_day;           /* day of the current touch cap, -1 = none */
    uint32_t daily_touch_coins;  /* 0..50 */
    int32_t feed_day;            /* day of the current feed cap, -1 = none */
    uint32_t feed_mask;          /* bits 0..5: that food already paid today */
    int32_t fortune_day;         /* day of the current fortune, -1 = never */
    uint32_t fortune;            /* 0..19, valid only if fortune_day >= 0 */
    int32_t decay_day;           /* last day living cost was settled, -1 = none */
    uint32_t decay_notice;       /* amount to announce once, 0 = nothing pending */
    int32_t sudoku_day;          /* day of the sudoku ledger, -1 = none */
    uint32_t sudoku_entry_paid;  /* 1 = today's sudoku entry fee is paid */
    uint32_t sudoku_reward_coins;/* 0..5, paid by today's solved puzzles */
    int32_t jiaobei_day;         /* day of the 掷圣杯 ledger, -1 = none */
    uint32_t jiaobei_count;      /* tosses settled today, 0..9 */
    uint32_t jiaobei_streak;     /* consecutive holy cups today, 0..9 */
    uint32_t jiaobei_last;       /* last result today: 0 none, 1 holy,
                                  * 2 laughing, 3 yin (the codes below) */
    int32_t tracing_day;         /* day of the 描红 ledger, -1 = none */
    uint32_t tracing_mask;       /* bits 0..9: that character already
                                  * paid today (same index as
                                  * MUSE_TRACING_TABLE) */
    int32_t luckywheel_day;      /* day of the 幸运大转盘 ledger, -1 = none */
    uint32_t luckywheel_count;   /* paid spins settled today, 0..9 */
} muse_pet_model_t;

/* Result codes carried in `arg` by PET_JIAOBEI_TOSS (same numbering as
 * muse_jiaobei_result_t -- the model does not include that header, so
 * the numbers are pinned here). 0 means "no result" (a bare
 * muse_pet_request(PET_JIAOBEI_TOSS), e.g. from the console): the toss
 * is charged and counted, but the streak and last result are untouched. */
#define MUSE_PET_JIAOBEI_SHENG 1
#define MUSE_PET_JIAOBEI_XIAO 2
#define MUSE_PET_JIAOBEI_YIN 3

/* Result codes carried in `arg` by PET_LUCKYWHEEL_SPIN (same numbering
 * as muse_luckywheel_result_t -- the model does not include that header,
 * so the numbers are pinned here). The low byte is the result; the
 * MUSE_PET_LUCKY_FREE bit marks a free re-spin (won by landing on
 * "再来一次"): no fee, no daily count. 0 means "no result" (a bare
 * request, e.g. from the console): the spin is charged and counted,
 * but no prize is paid. */
#define MUSE_PET_LUCKY_FREE 0x100
#define MUSE_PET_LUCKY_THANKS 1
#define MUSE_PET_LUCKY_COINS_2 2
#define MUSE_PET_LUCKY_COINS_5 3
#define MUSE_PET_LUCKY_COINS_10 4
#define MUSE_PET_LUCKY_AGAIN 5

/* Actions. PET_FEED_* settle identically; see the header note above.
 * PET_DECAY_ACK and PET_RESET are internal only: never sent by UI buttons.
 * PET_DECAY_ACK clears decay_notice after phase-2 UI has shown it once;
 * since 1.5b its arg is the announced decay_day, and only a notice for
 * that day is cleared (see muse_pet_decay_ack()).
 * PET_RESET rebuilds the initial state; only the Test-gated console command
 * may send it (see the muse_input.c patch note).
 * PET_FEE deducts one round's entry fee (MUSE_PET_GAME_STAKE) for a snake
 * or whack-a-mole game (user decision 2026-10-31): the UI gates on the
 * snapshot first, and the worker re-checks -- under MUSE_PET_GAME_STAKE
 * coins it returns PET_POOR and settles nothing, exactly like PET_GAME's
 * gate. It never changes the persisted layout.
 * PET_SUDOKU_ENTRY is the sudoku page's entry fee (user decision 2026-11-01):
 * charged at most ONCE per calendar day, idempotent -- a second request on
 * the same day is PET_OK and moves nothing, so flipping puzzles (上一题 /
 * 下一题) and 再玩一次 are all free. Under MUSE_PET_SUDOKU_ENTRY_COINS
 * and unpaid it returns PET_POOR, again without settling.
 * PET_SUDOKU_SOLVE is the completion reward (arg unused): it pays
 * MUSE_PET_SUDOKU_SOLVE_COINS (10) for the day's first completion and
 * PET_CAP for every later puzzle -- and for any day whose ledger already
 * holds a nonzero reward, including a v4 blob's 5, which is treated as
 * paid and never topped up to 10.
 * PET_FORTUNE draws the day's card (user decision 2026-11-14: the draw
 * costs MUSE_PET_FORTUNE_COST, once a day, and the card is a pure
 * function of the day -- the same day always draws the same card, on
 * any board, with no random input). Same-day repeats return
 * PET_FORTUNE_SAME and move nothing; under MUSE_PET_FORTUNE_COST a
 * first draw of the day returns PET_POOR and settles nothing.
 * PET_JIAOBEI_TOSS settles one 掷圣杯 toss (user decision 2026-11-14):
 * MUSE_PET_JIAOBEI_COST coins (2 since 2026-11-16), at most 9 a day
 * (MUSE_PET_JIAOBEI_DAILY_MAX). The 10th toss of the day returns
 * PET_CAP and settles nothing; under MUSE_PET_JIAOBEI_COST it returns
 * PET_POOR.
 * The outcome itself (holy / laughing / yin) is decided UI-side by
 * muse_jiaobei_toss() and rides in `arg` (MUSE_PET_JIAOBEI_*): a
 * settled holy toss extends the day's streak by 1, laughing / yin
 * clear it, and the last result is recorded -- so the board re-enters
 * the page showing today's last result and streak (user decision
 * 2026-11-14: the streak survives leaving the page within the same
 * day, and the midnight day boundary clears it). A refused toss
 * records nothing: the streak only moves for tosses that settled, and
 * a bare request (arg 0) charges and counts the toss without recording
 * a result. The toss itself is pure consumption with no payout.
 * PET_LUCKYWHEEL_SPIN settles one lucky wheel spin: MUSE_PET_LUCKYWHEEL_COST
 * coins (2) per paid spin, and NO daily cap any more (user decision
 * 2026-11-21, landed with 1.5.4d: the wheel spins as long as the wallet
 * pays; luckywheel_count is kept for the ledger only and never refuses).
 * Under MUSE_PET_LUCKYWHEEL_COST it returns PET_POOR. The result rides in
 * `arg` (MUSE_PET_LUCKY_*): +2 / +5 / +10 pay that many coins through
 * capped_add (never above the 100-coin wallet), THANKS and AGAIN pay
 * nothing, and the MUSE_PET_LUCKY_FREE bit marks a free re-spin won
 * by landing on "再来一次" -- no fee, no daily count, so it can
 * never hit PET_CAP or PET_POOR. A refused spin records nothing; a
 * bare request (arg 0) charges and counts the spin without paying a
 * prize. The prize is always within the 20-coin round ceiling, so no
 * extra clamp is needed.
 * PET_PLAY_REWARD settles a finished score-based round: arg is the coin
 * amount the UI converted with muse_pet_round_coins(), clamped here to
 * 0..MUSE_PET_ROUND_COIN_HARD_MAX (the wallet, 100 since 2026-11-17, was
 * 20) so a UI bug cannot mint coins. The round's net is that payout
 * minus the 10-coin entry fee the UI already charged with PET_FEE; the
 * model never nets the two itself.
 * PET_TRACING_SOLVE is the 描红 completion reward (user decision
 * 2026-11-20): arg is the character index (0..9, same as
 * MUSE_TRACING_TABLE); it pays MUSE_PET_TRACING_COINS (2) for the
 * day's first correct trace of that character and PET_CAP for every
 * later one -- the same once-a-day-per-item shape as the feed mask
 * (PET_FEED_*) and the sudoku reward (PET_SUDOKU_SOLVE), with the
 * tracing_mask ledger instead of a single flag. An out-of-range arg
 * settles nothing (PET_BUSY, like a bad sudoku day). The trace page
 * shows the praise either way; only the payout is once a day. */
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
    PET_JIAOBEI_TOSS,
    PET_TRACING_SOLVE,
    PET_LUCKYWHEEL_SPIN,
    /* Test only, console `>pet.coins=N`: set the wallet outright. Not
     * reachable from the UI, and the console command is Test-gated like
     * the other mutating pet.* ones. Bypasses decay like PET_RESET. */
    PET_SET_COINS,
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
                        * sudoku = today's 10 coins already paid, no coins */
    PET_POOR,          /* game/fee: coins < the entry fee, not settled,
                        * no round started */
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

/* Coins a finished round's score converts to: min(20, score), i.e. 1:1 with
 * the 20-coin ceiling. The only place the conversion curve lives, so snake
 * and whack-a-mole cannot drift apart. Call it with the round's final score
 * at OVER, never per point. The entry fee (10, charged at start through
 * PET_FEE) is NOT subtracted here: the UI shows payout and net separately,
 * so the conversion must stay the gross payout. */
uint32_t muse_pet_round_coins(uint32_t score);

/* Same 1:1 curve, but with the caller's ceiling (a game whose round
 * length differs from the standard one). The worker still clamps the
 * result to MUSE_PET_ROUND_COIN_HARD_MAX when it is paid out. */
uint32_t muse_pet_round_coins_cap(uint32_t score, uint32_t cap);

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

/*
 * Today's 掷圣杯 ledger, as a UI can read it without duplicating the
 * day rule: *remaining is how many tosses are left today
 * (MUSE_PET_JIAOBEI_DAILY_MAX minus today's settled count, or the full
 * 9 when no toss has been settled today). Derived from the snapshot;
 * nothing is written. The pointer may be NULL.
 */
void muse_pet_jiaobei_today(const muse_pet_model_t *s, int64_t now,
                           uint32_t *remaining);

/*
 * Today's 掷圣杯 streak + last result, as a UI can read them without
 * duplicating the day rule: *streak is the consecutive holy count
 * today (0 when nothing was tossed today or the last toss broke it),
 * *last is the last result today (0 = none, 1 = holy, 2 = laughing,
 * 3 = yin, the MUSE_PET_JIAOBEI_* codes). Both are 0 on a new day.
 * Derived from the snapshot; nothing is written. Either pointer may
 * be NULL.
 */
void muse_pet_jiaobei_result_today(const muse_pet_model_t *s, int64_t now,
                                   uint32_t *streak, uint32_t *last);

/*
 * Today's 描红 ledger, as a UI can read it without duplicating the
 * day rule: *paid_mask is the bits of the characters already paid
 * today (0..9, same index as MUSE_TRACING_TABLE; 0 on a new day or
 * when nothing has been traced today). Derived from the snapshot;
 * nothing is written. The pointer may be NULL.
 */
void muse_pet_tracing_today(const muse_pet_model_t *s, int64_t now,
                           uint32_t *paid_mask);

/*
 * Today's 幸运大转盘 ledger, as a UI can read it without duplicating
 * the day rule: *remaining is how many paid spins are left today
 * (MUSE_PET_LUCKYWHEEL_DAILY_MAX minus today's settled count, or the
 * full 9 when no spin has been settled today). Free re-spins do not
 * count against it. Derived from the snapshot; nothing is written.
 * The pointer may be NULL.
 */
void muse_pet_luckywheel_today(const muse_pet_model_t *s, int64_t now,
                               uint32_t *remaining);

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

/* Settle one action into `s` (decay is applied first). `random` is
 * currently unused by every action (PET_FORTUNE used to draw
 * random % 20; since 2026-11-14 the card is a pure function of the day,
 * so the parameter stays only to keep the signature stable). `arg`
 * carries the outcome of a game round (-1 lose / 0 draw / +1 win,
 * decided UI-side at click time, design §5.2) and is ignored by every
 * other action: since 1.5a a round costs 5 on a loss, pays 5 on a win
 * and costs nothing on a draw. */
muse_pet_result_t muse_pet_model_apply(muse_pet_model_t *s, muse_pet_action_t action,
                                       int64_t now, uint32_t random, int32_t arg);

/* True for the six feed actions. */
bool muse_pet_action_is_feed(muse_pet_action_t action);

/* 0..5 (rice / cake / coffee / hamburger / pizza / milk tea) for a feed
 * action, -1 for anything else. The
 * index is the bit in feed_mask, so each food pays once per day on its own. */
int muse_pet_feed_index(muse_pet_action_t action);

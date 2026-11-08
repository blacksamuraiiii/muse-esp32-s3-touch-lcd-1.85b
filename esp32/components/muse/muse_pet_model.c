/* Public-edition staging: local UI additions and/or privacy review; see source manifest. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet_model.c -- 1.5a gold-only economy.
 *
 * Adapted from TamaMe (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet_model.c, Apache-2.0).
 * What was deleted versus TamaMe, and why, is in muse_pet_model.h and in
 * PHASE1-NOTES.md: hunger / mood / energy, hourly decay, 36-second quanta,
 * the food cost/fullness table, conversation rewards and SFX state are gone.
 * The only recurring charge is the daily living cost (design §4.2).
 */
#include "muse_pet_model.h"

#include <string.h>

static uint32_t capped_add(uint32_t value, uint32_t amount)
{
    uint32_t sum = value + amount;
    return sum > MUSE_PET_COINS_MAX ? MUSE_PET_COINS_MAX : sum;
}

/* UTC day, TZ-independent (user decision 2026-10-25, phase 2).
 * now is the board UTC epoch from muse_time_start(); the day number is
 * (now + MUSE_PET_DAY_OFFSET_SECONDS) / 86400, i.e. days since 1970-01-01
 * UTC. With the default offset 0 the boundary is 08:00 Beijing time.
 * *calibrated is only the MUSE_PET_TIME_MIN gate: it still guards the
 * living-cost deduction and the daily-cap roll, but fortune no longer
 * consults it (see PET_FORTUNE below). */
int32_t muse_pet_day(int64_t now, bool *calibrated)
{
    if (calibrated) {
        *calibrated = now >= MUSE_PET_TIME_MIN;
    }
    if (now < 0) {
        return -1;
    }
    return (int32_t)((now + MUSE_PET_DAY_OFFSET_SECONDS) / 86400);
}

void muse_pet_model_init(muse_pet_model_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = MUSE_PET_VERSION;
    s->touch_day = -1;
    s->feed_day = -1;
    s->fortune_day = -1;
    s->decay_day = -1;
    s->sudoku_day = -1;
}

bool muse_pet_model_valid(const muse_pet_model_t *s)
{
    return s->version == MUSE_PET_VERSION &&
           s->coins <= MUSE_PET_COINS_MAX &&
           s->daily_touch_coins <= MUSE_PET_TOUCH_DAILY_MAX &&
           s->feed_mask < (1u << MUSE_PET_FEED_KINDS) &&
           s->fortune < MUSE_PET_FORTUNES &&
           s->touch_day >= -1 && s->feed_day >= -1 &&
           s->fortune_day >= -1 && s->decay_day >= -1 &&
           s->decay_notice <= MUSE_PET_COINS_MAX &&
           s->sudoku_day >= -1 &&
           s->sudoku_entry_paid <= 1 &&
           s->sudoku_reward_coins <= MUSE_PET_SUDOKU_DAILY_MAX;
}

uint32_t muse_pet_round_coins_cap(uint32_t score, uint32_t cap)
{
    /* Both guards are for retuning, not for today's constants: a 0 divisor
     * would fault and a cap of 0 would pay nothing, and -Werror builds must
     * stay warning-free whichever way the knobs move. */
    if (MUSE_PET_SCORE_PER_COIN == 0u || cap == 0u) {
        return 0;
    }
    uint32_t coins = score / MUSE_PET_SCORE_PER_COIN;
    return coins > cap ? cap : coins;
}

uint32_t muse_pet_round_coins(uint32_t score)
{
    return muse_pet_round_coins_cap(score, MUSE_PET_ROUND_COIN_MAX);
}

void muse_pet_sudoku_today(const muse_pet_model_t *s, int64_t now,
                           bool *entry_paid, uint32_t *reward_coins)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    (void)calibrated; /* the ledger is day-based only, see sudoku_roll() */
    bool same_day = s->sudoku_day != -1 && s->sudoku_day == day;
    if (entry_paid) {
        *entry_paid = same_day && s->sudoku_entry_paid != 0;
    }
    if (reward_coins) {
        *reward_coins = same_day ? s->sudoku_reward_coins : 0;
    }
}

/* Start a new sudoku ledger when the day advances. Unlike roll_cap() this
 * does not require a calibrated clock: the entry fee is a charge, and
 * "charge at most once a day" must hold on a board with no RTC too (the
 * price of that is one extra 5-coin charge if SNTP later jumps the day --
 * accepted, the alternative is a free sudoku forever on an unset clock).
 * A rollback (day < stored day) keeps the stored ledger, so a clock going
 * backwards cannot re-charge the same day twice. */
static void sudoku_roll(muse_pet_model_t *s, int32_t day)
{
    if (s->sudoku_day == day) {
        return;
    }
    if (s->sudoku_day != -1 && day < s->sudoku_day) {
        return; /* rollback: never re-charge, never re-grant */
    }
    s->sudoku_day = day;
    s->sudoku_entry_paid = 0;
    s->sudoku_reward_coins = 0;
}

/* Start a new daily cap when the calibrated day has advanced. On rollback
 * (day <= stored day) the stored cap is kept: a clock going backwards must
 * never re-grant a day's rewards, and must never erase today's progress. */
static void roll_cap(int32_t *day_field, uint32_t *daily, int32_t day, bool calibrated)
{
    if (!calibrated) {
        return; /* design §4.2: uncalibrated clock resets nothing */
    }
    if (*day_field == -1 || day > *day_field) {
        *day_field = day;
        *daily = 0;
    }
}

void muse_pet_model_decay(muse_pet_model_t *s, int64_t now)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    if (!calibrated) {
        return; /* design §4.2: uncalibrated clock deducts nothing */
    }
    if (s->decay_day == -1) {
        /* First calibrated run: establish the baseline, do not back-charge
         * days that predate any recorded state. */
        s->decay_day = day;
        return;
    }
    if (day <= s->decay_day) {
        return; /* same day, or clock rollback: no deduction, no refund */
    }
    uint32_t elapsed = (uint32_t)(day - s->decay_day);
    uint32_t due = elapsed * MUSE_PET_DECAY_PER_DAY;
    /* Floor at zero, never negative (design §4.2). The notice is the amount
     * actually taken, so the announced number always matches the deduction. */
    s->decay_notice = due >= s->coins ? s->coins : due;
    s->coins = due >= s->coins ? 0 : s->coins - due;
    s->decay_day = day;
}

void muse_pet_model_roll_daily(muse_pet_model_t *s, int64_t now)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    roll_cap(&s->feed_day, &s->feed_mask, day, calibrated);
    roll_cap(&s->touch_day, &s->daily_touch_coins, day, calibrated);
}

bool muse_pet_action_is_feed(muse_pet_action_t action)
{
    return action == PET_FEED_RICE || action == PET_FEED_CAKE ||
           action == PET_FEED_COFFEE || action == PET_FEED_BURGER ||
           action == PET_FEED_PIZZA || action == PET_FEED_MILKTEA;
}

int muse_pet_feed_index(muse_pet_action_t action)
{
    switch (action) {
    case PET_FEED_RICE: return 0;
    case PET_FEED_CAKE: return 1;
    case PET_FEED_COFFEE: return 2;
    case PET_FEED_BURGER: return 3;
    case PET_FEED_PIZZA: return 4;
    case PET_FEED_MILKTEA: return 5;
    default: return -1;
    }
}

muse_pet_result_t muse_pet_model_apply(muse_pet_model_t *s, muse_pet_action_t action,
                                       int64_t now, uint32_t random, int32_t arg)
{
    /* Internal actions bypass decay: reset means "fresh state, no
     * back-charge", and ack must not erase a notice that decay in this same
     * call would have just created (see PHASE1-NOTES.md §5). */
    if (action == PET_RESET) {
        muse_pet_model_init(s);
        return PET_OK;
    }
    if (action == PET_DECAY_ACK) {
        /* 1.5b: arg is the day the UI announced. Only clear a notice for
         * that day: an ACK that arrives after a midnight rollover created
         * a new notice must not silently clear the new one. A stale ACK
         * is a no-op, and the UI re-announces from the fresh snapshot. */
        if (arg == s->decay_day) {
            s->decay_notice = 0;
        }
        return PET_OK;
    }

    muse_pet_model_decay(s, now);
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);

    if (action == PET_TOUCH) {
        roll_cap(&s->touch_day, &s->daily_touch_coins, day, calibrated);
        if (s->daily_touch_coins >= MUSE_PET_TOUCH_DAILY_MAX) {
            return PET_CAP; /* happy only, no coins (design §4.3) */
        }
        s->daily_touch_coins += MUSE_PET_TOUCH_COINS;
        s->coins = capped_add(s->coins, MUSE_PET_TOUCH_COINS);
        return PET_COIN;
    }

    if (muse_pet_action_is_feed(action)) {
        /* 1.5a (user decision 2026-10-25), six foods since 2026-11-07: each
         * food pays its own 10 once a day -- 60 a day in total, not "the
         * first feed of the day". The mask bit is what the UI reads back
         * to word the result. */
        int kind = muse_pet_feed_index(action);
        roll_cap(&s->feed_day, &s->feed_mask, day, calibrated);
        if (kind < 0 || (s->feed_mask & (1u << kind)) != 0) {
            /* Still PET_OK by design §4.3: whether coins were added is read
             * from the snapshot's feed_mask, not from a failure code. */
            return PET_OK;
        }
        s->feed_mask |= (1u << kind);
        s->coins = capped_add(s->coins, MUSE_PET_FEED_COINS);
        return PET_OK;
    }

    if (action == PET_GAME) {
        /* 1.5a (user decision 2026-10-25): the stake is 5 either way -- a win
         * pays 5, a loss costs 5, a draw costs nothing. `arg` is the outcome
         * the UI already decided at click time; a round still cannot start
         * with fewer than 5 coins on hand, so PET_POOR keeps its meaning. */
        if (s->coins < MUSE_PET_GAME_STAKE) {
            return PET_POOR; /* not settled, no round started */
        }
        if (arg > 0) {
            s->coins = capped_add(s->coins, MUSE_PET_GAME_STAKE);
        } else if (arg < 0) {
            s->coins -= MUSE_PET_GAME_STAKE;
        }
        return PET_OK;
    }

    if (action == PET_FEE) {
        /* Entry fee for one snake or whack-a-mole game (user decision
         * 2026-10-31; sudoku moved to PET_SUDOKU_ENTRY on 2026-11-01 because
         * its fee is daily, not per puzzle): same 5-coin gate as PET_GAME, no
         * outcome, no refund path -- the UI only sends it after its own
         * snapshot gate passed, so PET_POOR here means the snapshot was stale.
         * `arg` is unused. */
        if (s->coins < MUSE_PET_GAME_STAKE) {
            return PET_POOR; /* not settled, no game started */
        }
        s->coins -= MUSE_PET_GAME_STAKE;
        return PET_OK;
    }

    if (action == PET_SUDOKU_ENTRY) {
        /* One ticket per calendar day (user decision 2026-11-01): the first
         * entry pays 5, every later entry / 上一题 / 下一题 / 再玩一次 that
         * day is free. Idempotent, so the UI can call it on every action
         * without a local "already paid" flag to get wrong. */
        sudoku_roll(s, day);
        if (s->sudoku_entry_paid) {
            return PET_OK; /* today's ticket is already in hand */
        }
        if (s->coins < MUSE_PET_GAME_STAKE) {
            return PET_POOR; /* not settled, nothing unlocks */
        }
        s->coins -= MUSE_PET_GAME_STAKE;
        s->sudoku_entry_paid = 1;
        return PET_OK;
    }

    if (action == PET_SUDOKU_SOLVE) {
        /* Completion reward, all-or-nothing per day (user decision
         * 2026-11-01, retuning 1.5f's v4): the day's first completion pays
         * MUSE_PET_SUDOKU_SOLVE_COINS (10) and every later puzzle pays
         * nothing (PET_CAP). The gate is "anything already paid", not
         * "room under the cap": a ledger that already holds a nonzero
         * reward counts as paid -- in particular a blob written by v4,
         * which stored 5 for its first solve, is NOT topped up to 10.
         * The daily maximum constant stays as the validation bound. */
        sudoku_roll(s, day);
        if (s->sudoku_reward_coins != 0) {
            return PET_CAP; /* today's sudoku coins are already paid */
        }
        s->sudoku_reward_coins = MUSE_PET_SUDOKU_SOLVE_COINS;
        s->coins = capped_add(s->coins, MUSE_PET_SUDOKU_SOLVE_COINS);
        return PET_OK;
    }

    if (action == PET_PLAY_REWARD) {
        /* End-of-round payout for snake / whack-a-mole (user decision
         * 2026-11-01, ceiling retuned to 10 the same day). The UI converts
         * its own score with muse_pet_round_coins(); the worker clamps
         * again so a UI bug cannot mint coins, and 0 is a valid settlement
         * (a scoreless round earns nothing) that still returns PET_OK.
         * This is the GROSS payout: the 5-coin entry fee was already
         * charged at round start through PET_FEE, so the round's net is
         * payout minus 5 and is the UI's to show, not the model's to
         * fold in. */
        uint32_t add = arg > 0 ? (uint32_t)arg : 0;
        if (add > MUSE_PET_ROUND_COIN_HARD_MAX) {
            add = MUSE_PET_ROUND_COIN_HARD_MAX;
        }
        if (add != 0) {
            s->coins = capped_add(s->coins, add);
        }
        return PET_OK;
    }

    if (action == PET_FORTUNE) {
        /* User decision 2026-10-25 (phase 2): fortune always uses the
         * current UTC day, calibrated or not -- no PET_TIME refusal.
         * Same UTC day shows the same card; a new UTC day draws a new one.
         * Consequence, accepted for preview: if SNTP later jumps the date,
         * the next draw is legitimately a new card for the jumped-to day. */
        (void)calibrated;
        if (s->fortune_day == day) {
            return PET_FORTUNE_SAME;
        }
        s->fortune_day = day;
        s->fortune = random % MUSE_PET_FORTUNES;
        /* 1.5a (user decision 2026-10-25): a new card pays 20. A same-day
         * repeat returns PET_FORTUNE_SAME above and pays nothing, so the
         * daily total is touch 50 + feed 30 + fortune 20 = 100. */
        s->coins = capped_add(s->coins, MUSE_PET_FORTUNE_COINS);
        return PET_FORTUNE_NEW;
    }

    return PET_BUSY;
}

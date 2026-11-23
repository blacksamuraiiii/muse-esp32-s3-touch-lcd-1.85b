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
    s->jiaobei_day = -1;
    s->tracing_day = -1;
    s->luckywheel_day = -1;
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
           s->sudoku_reward_coins <= MUSE_PET_SUDOKU_DAILY_MAX &&
           s->jiaobei_day >= -1 &&
           /* Structural bound only (MUSE_PET_JIAOBEI_LEDGER_MAX): valid()
            * checks that a stored blob is sane, not that it honours the
            * per-day policy cap -- a Test blob with more than 9 tosses
            * must still load on Release (2026-11-17, B2). */
           s->jiaobei_count <= MUSE_PET_JIAOBEI_LEDGER_MAX &&
           s->jiaobei_streak <= MUSE_PET_JIAOBEI_LEDGER_MAX &&
           s->jiaobei_last <= 3 &&
           s->tracing_day >= -1 &&
           s->tracing_mask < (1u << MUSE_PET_TRACING_CHARS) &&
           s->luckywheel_day >= -1 &&
           /* Structural bound only (MUSE_PET_LUCKYWHEEL_LEDGER_MAX):
            * same B2 reasoning as the 掷圣杯 ledger above. */
           s->luckywheel_count <= MUSE_PET_LUCKYWHEEL_LEDGER_MAX;
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

/* Start a new 掷圣杯 ledger when the day advances. Same rule as
 * sudoku_roll (no calibrated clock required): "at most 9 tosses a day"
 * must hold on a board with no RTC too, and a rollback keeps the stored
 * ledger so a clock going backwards cannot re-grant tosses. The streak
 * and last result roll with the count: a new day starts at 0 / none. */
static void jiaobei_roll(muse_pet_model_t *s, int32_t day)
{
    if (s->jiaobei_day == day) {
        return;
    }
    if (s->jiaobei_day != -1 && day < s->jiaobei_day) {
        return; /* rollback: never re-grant */
    }
    s->jiaobei_day = day;
    s->jiaobei_count = 0;
    s->jiaobei_streak = 0;
    s->jiaobei_last = 0;
}

void muse_pet_jiaobei_today(const muse_pet_model_t *s, int64_t now,
                           uint32_t *remaining)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    (void)calibrated; /* the ledger is day-based only, see jiaobei_roll() */
    if (remaining) {
        bool same_day = s->jiaobei_day != -1 && s->jiaobei_day == day;
        uint32_t used = same_day ? s->jiaobei_count : 0;
        *remaining = used >= MUSE_PET_JIAOBEI_DAILY_MAX
                          ? 0
                          : MUSE_PET_JIAOBEI_DAILY_MAX - used;
    }
}

void muse_pet_jiaobei_result_today(const muse_pet_model_t *s, int64_t now,
                                   uint32_t *streak, uint32_t *last)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    (void)calibrated; /* the ledger is day-based only, see jiaobei_roll() */
    bool same_day = s->jiaobei_day != -1 && s->jiaobei_day == day;
    if (streak) {
        *streak = same_day ? s->jiaobei_streak : 0;
    }
    if (last) {
        *last = same_day ? s->jiaobei_last : 0;
    }
}

/* Start a new 描红 ledger when the day advances. Same rule as
 * sudoku_roll (no calibrated clock required): "one payout per
 * character per day" must hold on a board with no RTC too, and a
 * rollback keeps the stored ledger so a clock going backwards cannot
 * re-grant a character. The mask rolls with the day: a new day
 * starts with no character paid. */
static void tracing_roll(muse_pet_model_t *s, int32_t day)
{
    if (s->tracing_day == day) {
        return;
    }
    if (s->tracing_day != -1 && day < s->tracing_day) {
        return; /* rollback: never re-grant */
    }
    s->tracing_day = day;
    s->tracing_mask = 0;
}

void muse_pet_tracing_today(const muse_pet_model_t *s, int64_t now,
                           uint32_t *paid_mask)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    (void)calibrated; /* the ledger is day-based only, see tracing_roll() */
    if (paid_mask) {
        bool same_day = s->tracing_day != -1 && s->tracing_day == day;
        *paid_mask = same_day ? s->tracing_mask : 0;
    }
}

/* The lucky wheel ledger rolls like 掷筊's: a new day starts at 0, and
 * a rollback keeps the stored ledger so a clock going backwards cannot
 * re-grant spins. Free re-spins never touch the count. */
static void luckywheel_roll(muse_pet_model_t *s, int32_t day)
{
    if (s->luckywheel_day == day) {
        return;
    }
    if (s->luckywheel_day != -1 && day < s->luckywheel_day) {
        return; /* rollback: never re-grant */
    }
    s->luckywheel_day = day;
    s->luckywheel_count = 0;
}


/* The day's fortune card, a pure function of the day number (user
 * decision 2026-11-14): the same day always draws the same card, on any
 * board, with no random input. A Fibonacci hash, not day % 20: the plain
 * remainder cycles the 20 cards in order over 20 days, which reads as
 * rigged; the hash spreads consecutive days across the deck (15 distinct
 * cards over 20 consecutive days, checked in the host test). */
static uint32_t fortune_for_day(int32_t day)
{
    return ((uint32_t)day * 2654435761u) % MUSE_PET_FORTUNES;
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
     * actually taken, so the announced number always matches the deduction.
     * It ACCUMULATES across un-ACKed days (audit 2026-11-08): a plain
     * assignment overwrote yesterday's un-announced notice with today's,
     * so the earlier day's deduction was never announced. capped_add keeps
     * it within the wallet cap, so muse_pet_model_valid() still holds. */
    uint32_t taken = due >= s->coins ? s->coins : due;
    s->decay_notice = capped_add(s->decay_notice, taken);
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
    /* No action uses `random` any more (PET_FORTUNE drew random % 20
     * until 2026-11-14; the card is now a pure function of the day).
     * The parameter stays to keep the signature stable. */
    (void)random;
    /* Internal actions bypass decay: reset means "fresh state, no
     * back-charge", and ack must not erase a notice that decay in this same
     * call would have just created (see PHASE1-NOTES.md §5). */
    if (action == PET_RESET) {
        muse_pet_model_init(s);
        return PET_OK;
    }
    if (action == PET_SET_COINS) {
        /* Test only (console >pet.coins=N). Clamped, never negative, and
         * no decay: the point is an exact balance to test prices against. */
        if (arg < 0) {
            return PET_BUSY;
        }
        s->coins = (uint32_t)arg > MUSE_PET_COINS_MAX ? MUSE_PET_COINS_MAX
                                                     : (uint32_t)arg;
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
        /* Rock-paper-scissors, unified with the other games (user decision
         * 2026-11-08): a round costs the entry fee up front, then pays back
         * by outcome -- win MUSE_PET_ROUND_COIN_MAX (20), draw
         * MUSE_PET_GAME_STAKE (10), loss nothing. Net: win +10, draw 0,
         * loss -10. `arg` is the outcome the UI already decided at click
         * time; a round still cannot start with fewer than the entry fee
         * on hand, so PET_POOR keeps its meaning. */
        if (s->coins < MUSE_PET_GAME_STAKE) {
            return PET_POOR; /* not settled, no round started */
        }
        s->coins -= MUSE_PET_GAME_STAKE;
        if (arg > 0) {
            s->coins = capped_add(s->coins, MUSE_PET_ROUND_COIN_MAX);
        } else if (arg == 0) {
            s->coins = capped_add(s->coins, MUSE_PET_GAME_STAKE);
        }
        return PET_OK;
    }

    if (action == PET_FEE) {
        /* Entry fee for one of the six games (user decision
         * 2026-10-31; sudoku moved to PET_SUDOKU_ENTRY on 2026-11-01 because
         * its fee is daily, not per puzzle): same 10-coin gate as PET_GAME, no
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
         * without a local "already paid" flag to get wrong. The fee is
         * MUSE_PET_SUDOKU_ENTRY_COINS, decoupled from the other games'
         * entry fee: sudoku does not follow the 2026-11-08 retune. */
        sudoku_roll(s, day);
        if (s->sudoku_entry_paid) {
            return PET_OK; /* today's ticket is already in hand */
        }
        if (s->coins < MUSE_PET_SUDOKU_ENTRY_COINS) {
            return PET_POOR; /* not settled, nothing unlocks */
        }
        s->coins -= MUSE_PET_SUDOKU_ENTRY_COINS;
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
        /* End-of-round payout for any of the six games (user decision
         * 2026-11-01, ceiling retuned to 10 the same day, then to 20 on
         * 2026-11-08). The UI converts its own score with
         * muse_pet_round_coins() (OX棋 maps its outcome instead, with
         * muse_tictactoe_payout_coins()); the worker clamps again so a
         * UI bug cannot mint coins, and 0 is a valid settlement (a
         * scoreless round earns nothing) that still returns PET_OK.
         * This is the GROSS payout: the 10-coin entry fee was already
         * charged at round start through PET_FEE, so the round's net
         * is payout minus 10 and is the UI's to show, not the model's
         * to fold in. */
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
         * the next draw is legitimately a new card for the jumped-to day.
         * User decision 2026-11-14 (price 5 -> 2 on 2026-11-16): the draw
         * costs MUSE_PET_FORTUNE_COST (it used to pay 20),
         * once a day, and the card is a pure function of the day
         * (fortune_for_day) -- the same day always draws the same card,
         * on any board, with no random input. Under
         * MUSE_PET_FORTUNE_COST a first draw
         * of the day is refused (PET_POOR) and settles nothing, so the
         * balance never goes negative. */
        (void)calibrated;
        if (s->fortune_day == day) {
            return PET_FORTUNE_SAME;
        }
        if (s->coins < MUSE_PET_FORTUNE_COST) {
            return PET_POOR; /* not settled, no card drawn */
        }
        s->coins -= MUSE_PET_FORTUNE_COST;
        s->fortune_day = day;
        s->fortune = fortune_for_day(day);
        return PET_FORTUNE_NEW;
    }

    if (action == PET_JIAOBEI_TOSS) {
        /* One 掷圣杯 toss (user decision 2026-11-14): pure consumption,
         * MUSE_PET_JIAOBEI_COST, at most MUSE_PET_JIAOBEI_DAILY_MAX a day.
         * The ledger rolls like sudoku's (no
         * calibrated clock required: "at most 9 a day" must hold on a
         * board with no RTC too). The cap is checked first: a player
         * with coins but no tosses left gets PET_CAP, not PET_POOR.
         * The result rides in `arg` (MUSE_PET_JIAOBEI_*): a settled
         * holy toss extends the day's streak, laughing / yin clear it,
         * and the last result is recorded -- so the board re-enters the
         * page showing today's last result and streak. A refused toss
         * returns before this point and records nothing; a bare request
         * (arg 0) charges and counts the toss without recording a
         * result. The streak can never exceed the day's toss count
         * (9), so the valid() bound holds by construction. */
        jiaobei_roll(s, day);
        if (s->jiaobei_count >= MUSE_PET_JIAOBEI_DAILY_MAX) {
            return PET_CAP; /* today's 9 tosses are used up */
        }
        if (s->coins < MUSE_PET_JIAOBEI_COST) {
            return PET_POOR; /* not settled, no toss */
        }
        s->coins -= MUSE_PET_JIAOBEI_COST;
        s->jiaobei_count++;
        if (arg == MUSE_PET_JIAOBEI_SHENG) {
            s->jiaobei_streak++;
            s->jiaobei_last = (uint32_t)MUSE_PET_JIAOBEI_SHENG;
        } else if (arg == MUSE_PET_JIAOBEI_XIAO) {
            s->jiaobei_streak = 0;
            s->jiaobei_last = (uint32_t)MUSE_PET_JIAOBEI_XIAO;
        } else if (arg == MUSE_PET_JIAOBEI_YIN) {
            s->jiaobei_streak = 0;
            s->jiaobei_last = (uint32_t)MUSE_PET_JIAOBEI_YIN;
        }
        return PET_OK;
    }

    if (action == PET_TRACING_SOLVE) {
        /* One 描红 completion reward (user decision 2026-11-20):
         * MUSE_PET_TRACING_COINS (2) for the day's first correct trace
         * of the character in `arg`, PET_CAP for every later trace of
         * the same character the same day -- the once-a-day-per-item
         * shape of the feed mask. The ledger rolls like sudoku's (no
         * calibrated clock required). An out-of-range arg settles
         * nothing: the UI only ever sends 0..29, and a stray value
         * must not mint coins or corrupt the mask. The trace page
         * shows the praise either way; only the payout is once a day. */
        if (arg < 0 || arg >= (int)MUSE_PET_TRACING_CHARS) {
            return PET_BUSY;
        }
        tracing_roll(s, day);
        uint32_t bit = 1u << (uint32_t)arg;
        if ((s->tracing_mask & bit) != 0) {
            return PET_CAP; /* this character already paid today */
        }
        s->tracing_mask |= bit;
        s->coins = capped_add(s->coins, MUSE_PET_TRACING_COINS);
        return PET_OK;
    }

    if (action == PET_LUCKYWHEEL_SPIN) {
        /* One lucky wheel spin (user decision 2026-11-20): a paid spin
         * costs MUSE_PET_LUCKYWHEEL_COST. No daily cap any more (user
         * decision 2026-11-21): the wheel spins as long as the wallet
         * pays -- the old 9-a-day cap and its PET_CAP are gone (M-3).
         * The ledger still rolls like 掷筊's (no calibrated clock
         * required) and counts every paid spin for the record. The
         * result rides in `arg` (MUSE_PET_LUCKY_*):
         * +2 / +5 / +10 pay through capped_add, THANKS and AGAIN pay
         * nothing, and the MUSE_PET_LUCKY_FREE bit marks a free
         * re-spin won by landing on "再来一次" -- no fee, no daily
         * count, so it can never hit PET_CAP or PET_POOR, and it
         * still pays its prize. A refused spin returns before this
         * point and records nothing; a bare request (arg 0) charges
         * and counts the spin without paying a prize. */
        bool free = (arg & MUSE_PET_LUCKY_FREE) != 0;
        int32_t result = arg & 0xFF;
        luckywheel_roll(s, day);
        if (!free) {
            /* No daily cap any more (user decision 2026-11-21): the wheel
             * spins as long as the wallet pays. The count is still kept
             * for the ledger and the harness, it just never refuses. */
            if (s->coins < MUSE_PET_LUCKYWHEEL_COST) {
                return PET_POOR; /* not settled, no spin */
            }
            s->coins -= MUSE_PET_LUCKYWHEEL_COST;
            s->luckywheel_count++;
        }
        if (result == MUSE_PET_LUCKY_COINS_2) {
            s->coins = capped_add(s->coins, 2);
        } else if (result == MUSE_PET_LUCKY_COINS_5) {
            s->coins = capped_add(s->coins, 5);
        } else if (result == MUSE_PET_LUCKY_COINS_10) {
            s->coins = capped_add(s->coins, 10);
        }
        return PET_OK;
    }

    return PET_BUSY;
}

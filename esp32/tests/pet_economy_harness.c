/* Pet economy harness: checks the coin rules in muse_pet_model.c without
 * LVGL, ESP-IDF or NVS (the model file only includes <string.h>).
 * Run by tests/test_pet_economy.py. Prints "ok" on success; any failed
 * check aborts via assert.
 *
 * What it pins down (user decision 2026-11-01, retuned the same day after
 * 1.5f shipped the 5-coin ceiling as "v4"):
 *   - muse_pet_round_coins(): 1 point = 1 coin, 10 coins per round ceiling,
 *     and the same curve for every score-based game.
 *   - A full score round: 5-coin entry fee up front (PET_FEE) plus the
 *     payout at the end, so the net is payout minus 5 -- 10 points = +5,
 *     5 points = 0 (a draw), 0 points = -5 (a loss), 15 points = +5
 *     (capped), exactly the rock-paper-scissors outcomes.
 *   - PET_PLAY_REWARD: clamped to the 10-coin ceiling, additive, never
 *     above the wallet cap.
 *   - PET_SUDOKU_ENTRY: once per calendar day, idempotent, PET_POOR under 5.
 *   - PET_SUDOKU_SOLVE: 10 coins for the day's first puzzle, PET_CAP after;
 *     a ledger already holding a nonzero reward -- including a v4 blob's 5
 *     -- counts as paid and is never topped up to 10.
 *   - Day rollback (clock going backwards) never re-charges a paid day.
 *   - muse_pet_sudoku_today() answers the UI's two questions.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "muse_pet_model.h"

/* Day N as a UTC epoch: 00:00 plus a bit, so the value is unambiguously
 * inside that day. MUSE_PET_DAY_OFFSET_SECONDS is 8*3600 (local midnight),
 * so day = (now + 28800) / 86400; 3600 + 28800 = 32400 < 86400, so this
 * still lands inside day N. */
static int64_t day_now(int32_t day)
{
    return (int64_t)day * 86400 + 3600;
}

static muse_pet_result_t apply(muse_pet_model_t *m, muse_pet_action_t a,
                               int32_t day, int32_t arg)
{
    return muse_pet_model_apply(m, a, day_now(day), 0, arg);
}

int main(void)
{
    /* --- the conversion curve ------------------------------------------ */
    assert(MUSE_PET_ROUND_COIN_MAX == 10);
    assert(MUSE_PET_SUDOKU_DAILY_MAX == 10);
    assert(MUSE_PET_SUDOKU_SOLVE_COINS == 10);
    assert(MUSE_PET_SCORE_PER_COIN == 1);
    assert(muse_pet_round_coins(0) == 0);
    /* 1:1 below the ceiling -- this is the behaviour a 13-point snake round
     * was missing when the divisor was 5 (it paid 2 coins). */
    assert(muse_pet_round_coins(1) == 1);
    assert(muse_pet_round_coins(4) == 4);
    assert(muse_pet_round_coins(5) == 5);
    assert(muse_pet_round_coins(9) == 9);
    /* 10 is the ceiling: an ordinary 30 s whack-a-mole round (~28 hits)
     * and a long snake run both stop here, so the payout never exceeds
     * twice the 5-coin entry fee. */
    assert(muse_pet_round_coins(10) == 10);
    assert(muse_pet_round_coins(13) == 10);
    assert(muse_pet_round_coins(15) == 10);
    assert(muse_pet_round_coins(24) == 10);
    assert(muse_pet_round_coins(25) == 10);
    assert(muse_pet_round_coins(26) == 10);
    assert(muse_pet_round_coins(28) == 10);
    assert(muse_pet_round_coins(1000) == 10);

    /* --- round payout -------------------------------------------------- */
    const int32_t d1 = 20400; /* 2025-11-08-ish: above MUSE_PET_TIME_MIN */
    muse_pet_model_t m;
    muse_pet_model_init(&m);
    assert(muse_pet_model_valid(&m));
    m.coins = 10;
    assert(apply(&m, PET_PLAY_REWARD, d1, 3) == PET_OK);
    assert(m.coins == 13);
    /* Clamped to the per-round ceiling, even if the UI asks for more. */
    assert(apply(&m, PET_PLAY_REWARD, d1, 9) == PET_OK);
    assert(m.coins == 22);
    assert(apply(&m, PET_PLAY_REWARD, d1, 15) == PET_OK);
    assert(m.coins == 32); /* 15 asked, 10 paid */
    /* A negative arg is a zero payout, not a refund. */
    assert(apply(&m, PET_PLAY_REWARD, d1, -4) == PET_OK);
    assert(m.coins == 32);
    /* 0 is a valid settlement (a scoreless round) and changes nothing. */
    assert(apply(&m, PET_PLAY_REWARD, d1, 0) == PET_OK);
    assert(m.coins == 32);
    /* The wallet cap still applies. */
    m.coins = MUSE_PET_COINS_MAX - 2;
    assert(apply(&m, PET_PLAY_REWARD, d1, 5) == PET_OK);
    assert(m.coins == MUSE_PET_COINS_MAX);

    /* --- a full score round: fee up front, payout at the end ----------- */
    /* 10 points: fee 5, payout 10, net +5 -- the rock-paper-scissors win. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(m.coins == 15);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(10)) == PET_OK);
    assert(m.coins == 25);
    /* 5 points: fee 5, payout 5, net 0 -- the draw. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(5)) == PET_OK);
    assert(m.coins == 20);
    /* 0 points: fee 5, payout 0, net -5 -- the loss. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(0)) == PET_OK);
    assert(m.coins == 15);
    /* 15 points: fee 5, payout capped at 10, net +5 -- the ceiling binds. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(15)) == PET_OK);
    assert(m.coins == 25);
    /* Under 5 coins the fee is refused and nothing is settled. */
    muse_pet_model_init(&m);
    m.coins = 4;
    assert(apply(&m, PET_FEE, d1, 0) == PET_POOR);
    assert(m.coins == 4);

    /* --- sudoku: one entry fee per day --------------------------------- */
    muse_pet_model_init(&m);
    m.coins = 40;
    assert(apply(&m, PET_SUDOKU_ENTRY, d1, 0) == PET_OK);
    assert(m.coins == 35); /* 5 charged */
    assert(m.sudoku_entry_paid == 1);
    assert(m.sudoku_day == d1);
    /* Idempotent: every later entry / 上一题 / 下一题 / 再玩一次 that day is
     * free, so the same request must not deduct again. */
    assert(apply(&m, PET_SUDOKU_ENTRY, d1, 0) == PET_OK);
    assert(m.coins == 35);
    bool paid = false;
    uint32_t got = 0;
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(paid);
    assert(got == 0);

    /* --- sudoku: 10 coins for the first solved puzzle, then nothing ---- */
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_OK);
    assert(m.coins == 45);
    assert(m.sudoku_reward_coins == 10);
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_CAP);
    assert(m.coins == 45); /* second and later puzzles pay nothing */
    assert(m.sudoku_reward_coins == 10);
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(paid);
    assert(got == 10);

    /* --- a new day rolls the sudoku ledger ----------------------------- */
    const int32_t d2 = d1 + 1;
    /* The living cost (20) is settled before the action, as for every other
     * action, so 45 - 20 - 5 = 20. */
    assert(apply(&m, PET_SUDOKU_ENTRY, d2, 0) == PET_OK);
    assert(m.coins == 20);
    assert(m.sudoku_day == d2);
    assert(m.sudoku_reward_coins == 0);
    assert(apply(&m, PET_SUDOKU_SOLVE, d2, 0) == PET_OK);
    assert(m.coins == 30);

    /* --- clock rollback never re-charges a paid day -------------------- */
    uint32_t before = m.coins;
    assert(apply(&m, PET_SUDOKU_ENTRY, d2 - 1, 0) == PET_OK);
    assert(m.coins == before);  /* no second ticket sale for the same day */
    assert(m.sudoku_day == d2); /* the stored day is kept, not moved back */

    /* --- under 5 coins the entry is refused and nothing is settled ----- */
    muse_pet_model_init(&m);
    m.coins = 4;
    assert(apply(&m, PET_SUDOKU_ENTRY, d1, 0) == PET_POOR);
    assert(m.coins == 4);
    assert(m.sudoku_entry_paid == 0);

    /* --- a v4 blob's stored 5 counts as paid, never topped up --------- */
    muse_pet_model_init(&m);
    m.coins = 50;
    m.sudoku_day = d1;
    m.sudoku_entry_paid = 1;
    m.sudoku_reward_coins = 5; /* 1.5f's v4 first-solve grant */
    assert(muse_pet_model_valid(&m)); /* 5 is still within the 10 bound */
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_CAP);
    assert(m.coins == 50);
    assert(m.sudoku_reward_coins == 5);
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(got == 5); /* reads back 5; the UI treats nonzero as paid */

    /* --- validation catches a corrupt ledger --------------------------- */
    muse_pet_model_init(&m);
    assert(muse_pet_model_valid(&m));
    m.sudoku_reward_coins = MUSE_PET_SUDOKU_DAILY_MAX + 1;
    assert(!muse_pet_model_valid(&m));
    muse_pet_model_init(&m);
    m.sudoku_entry_paid = 2;
    assert(!muse_pet_model_valid(&m));

    /* --- roll_daily: the 2026-11-07 feed bug --------------------------- */
    /* Feed all three foods on d1, then simulate the worker tick on d2
     * (no feed action yet): the mask must already be clear, so the UI's
     * snapshot reads "nothing fed today" and the first rice tap of d2
     * answers "+10", not "already fed today". Before this fix the mask
     * was only rolled inside the feed action, so the first tap of a new
     * day read yesterday's mask (0b111) for s_last_feed_before. */
    muse_pet_model_init(&m);
    m.coins = 0;
    assert(apply(&m, PET_FEED_RICE, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_CAKE, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_COFFEE, d1, 0) == PET_OK);
    assert(m.coins == 30);
    assert(m.feed_day == d1);
    assert(m.feed_mask == 0x7u);
    /* The tick rolls the caps with no action at all. */
    muse_pet_model_roll_daily(&m, day_now(d2));
    assert(m.feed_day == d2);
    assert(m.feed_mask == 0);
    assert(m.coins == 30); /* rolling grants nothing, it only clears */
    /* Rice on the new day now pays, and the bit it sets is fresh.
     * (The 20-coin living cost is settled by the feed's decay first:
     * 30 - 20 + 10 = 20.) */
    assert(apply(&m, PET_FEED_RICE, d2, 0) == PET_OK);
    assert(m.coins == 20);
    assert(m.feed_mask == 0x1u);
    /* A second rice tap the same day pays nothing (mask bit already set). */
    assert(apply(&m, PET_FEED_RICE, d2, 0) == PET_OK);
    assert(m.coins == 20);
    assert(m.feed_mask == 0x1u);

    /* --- roll_daily also clears the touch count ------------------------ */
    muse_pet_model_init(&m);
    m.coins = 0;
    for (int i = 0; i < 10; i++) {
        assert(apply(&m, PET_TOUCH, d1, 0) == PET_COIN);
    }
    assert(m.daily_touch_coins == MUSE_PET_TOUCH_DAILY_MAX);
    assert(apply(&m, PET_TOUCH, d1, 0) == PET_CAP); /* capped for d1 */
    muse_pet_model_roll_daily(&m, day_now(d2));
    assert(m.touch_day == d2);
    assert(m.daily_touch_coins == 0);
    assert(apply(&m, PET_TOUCH, d2, 0) == PET_COIN); /* fresh day, pays */

    /* --- roll_daily is idempotent and rollback-safe -------------------- */
    /* Same day: nothing changes. */
    uint32_t mask_before = m.feed_mask;
    muse_pet_model_roll_daily(&m, day_now(d2));
    assert(m.feed_mask == mask_before);
    assert(m.feed_day == d2);
    /* Clock going backwards: the stored day and mask are kept, so a
     * rollback can never re-grant a day's rewards. */
    muse_pet_model_roll_daily(&m, day_now(d2 - 1));
    assert(m.feed_day == d2);
    assert(m.feed_mask == mask_before);

    /* --- roll_daily is a no-op on an uncalibrated clock ---------------- */
    muse_pet_model_init(&m);
    m.feed_day = d1;
    m.feed_mask = 0x7u;
    muse_pet_model_roll_daily(&m, MUSE_PET_TIME_MIN - 3600);
    assert(m.feed_day == d1);
    assert(m.feed_mask == 0x7u);

    printf("ok\n");
    return 0;
}

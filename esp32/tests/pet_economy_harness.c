/* Pet economy harness: checks the coin rules in muse_pet_model.c without
 * LVGL, ESP-IDF or NVS (the model file only includes <string.h>).
 * Run by tests/test_pet_economy.py. Prints "ok" on success; any failed
 * check aborts via assert.
 *
 * What it pins down (user decision 2026-11-01, curve corrected the same day):
 *   - muse_pet_round_coins(): 1 point = 1 coin, 5 coins per round ceiling,
 *     and the same curve for every score-based game.
 *   - PET_PLAY_REWARD: clamped, additive, never above the wallet cap.
 *   - PET_SUDOKU_ENTRY: once per calendar day, idempotent, PET_POOR under 5.
 *   - PET_SUDOKU_SOLVE: 5 coins for the day's first puzzle, PET_CAP after.
 *   - Day rollback (clock going backwards) never re-charges a paid day.
 *   - muse_pet_sudoku_today() answers the UI's two questions.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "muse_pet_model.h"

/* Day N as a UTC epoch: 00:00 plus a bit, so the value is unambiguously
 * inside that day. MUSE_PET_DAY_OFFSET_SECONDS is 0, so day = now / 86400. */
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
    assert(MUSE_PET_ROUND_COIN_MAX == 5);
    assert(MUSE_PET_SUDOKU_DAILY_MAX == 5);
    assert(MUSE_PET_SUDOKU_SOLVE_COINS == 5);
    assert(MUSE_PET_SCORE_PER_COIN == 1);
    assert(muse_pet_round_coins(0) == 0);
    /* 1:1 below the ceiling -- this is the behaviour a 13-point snake round
     * was missing when the divisor was 5 (it paid 2 coins). */
    assert(muse_pet_round_coins(1) == 1);
    assert(muse_pet_round_coins(4) == 4);
    /* 5 is the ceiling: an ordinary 30 s whack-a-mole round (~28 hits) and
     * an ordinary snake run both stop at the 5-coin entry fee. */
    assert(muse_pet_round_coins(5) == 5);
    assert(muse_pet_round_coins(9) == 5);
    assert(muse_pet_round_coins(10) == 5);
    assert(muse_pet_round_coins(13) == 5);
    assert(muse_pet_round_coins(15) == 5);
    assert(muse_pet_round_coins(24) == 5);
    assert(muse_pet_round_coins(25) == 5);
    assert(muse_pet_round_coins(26) == 5);
    assert(muse_pet_round_coins(28) == 5);
    assert(muse_pet_round_coins(1000) == 5);

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
    assert(m.coins == 18);
    /* A negative arg is a zero payout, not a refund. */
    assert(apply(&m, PET_PLAY_REWARD, d1, -4) == PET_OK);
    assert(m.coins == 18);
    /* 0 is a valid settlement (a round under 5 points) and changes nothing. */
    assert(apply(&m, PET_PLAY_REWARD, d1, 0) == PET_OK);
    assert(m.coins == 18);
    /* The wallet cap still applies. */
    m.coins = MUSE_PET_COINS_MAX - 2;
    assert(apply(&m, PET_PLAY_REWARD, d1, 5) == PET_OK);
    assert(m.coins == MUSE_PET_COINS_MAX);

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

    /* --- sudoku: 5 coins for the first solved puzzle, then nothing ----- */
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_OK);
    assert(m.coins == 40);
    assert(m.sudoku_reward_coins == 5);
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_CAP);
    assert(m.coins == 40); /* second and later puzzles pay nothing */
    assert(m.sudoku_reward_coins == 5);
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(paid);
    assert(got == 5);

    /* --- a new day rolls the sudoku ledger ----------------------------- */
    const int32_t d2 = d1 + 1;
    /* The living cost (20) is settled before the action, as for every other
     * action, so 40 - 20 - 5 = 15. */
    assert(apply(&m, PET_SUDOKU_ENTRY, d2, 0) == PET_OK);
    assert(m.coins == 15);
    assert(m.sudoku_day == d2);
    assert(m.sudoku_reward_coins == 0);
    assert(apply(&m, PET_SUDOKU_SOLVE, d2, 0) == PET_OK);
    assert(m.coins == 20);

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

    /* --- validation catches a corrupt ledger --------------------------- */
    muse_pet_model_init(&m);
    assert(muse_pet_model_valid(&m));
    m.sudoku_reward_coins = MUSE_PET_SUDOKU_DAILY_MAX + 1;
    assert(!muse_pet_model_valid(&m));
    muse_pet_model_init(&m);
    m.sudoku_entry_paid = 2;
    assert(!muse_pet_model_valid(&m));

    printf("ok\n");
    return 0;
}

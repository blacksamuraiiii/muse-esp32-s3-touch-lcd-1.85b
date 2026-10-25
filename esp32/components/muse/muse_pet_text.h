/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet_text.h -- 1.5a static Chinese text tables.
 *
 * Self-written for 1.5a. NOT copied from TamaMe: TamaMe's cards and grades
 * lived behind muse_i18n.h keys (MUSE_KEY_s200...), which this repository
 * does not have (design §4.5), and its card texts were never imported here.
 * No 六爻 content: the six-lines port was shelved by user decision
 * 2026-10-24 and must not reappear as fortune cards.
 *
 * Layout contract (design §5.2, fortune subpage): one dark card, gold grade
 * on top, white body centred at 18pt. Bodies are kept short (aim <= 32
 * characters, hard limit 2-3 lines in the 360px card) so all 20 cards fit
 * without truncating the grade. Card texts are still subject to user review
 * (PHASE1-NOTES.md §6): they are draft copy, not approved final wording.
 * Grades are all non-negative by design choice for a family companion:
 * no 凶 card in 1.5a; add one only if the user asks for it.
 */
#pragma once

#include <stdint.h>

#define MUSE_PET_FOOD_RICE 0u
#define MUSE_PET_FOOD_CAKE 1u
#define MUSE_PET_FOOD_COFFEE 2u
#define MUSE_PET_FOOD_COUNT 3u

/* Number of fortune cards. Must equal MUSE_PET_FORTUNES (model header). */
uint32_t muse_pet_fortune_count(void);

/* Grade / body for card `index`. NULL when index is out of range. */
const char *muse_pet_fortune_grade(uint32_t index);
const char *muse_pet_fortune_text(uint32_t index);

/* Food name for a MUSE_PET_FOOD_* index. NULL when out of range. */
const char *muse_pet_food_name(uint32_t index);

/* Public-edition staging: local UI additions and/or privacy review; see source manifest. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet.h -- 1.5a pet worker API.
 *
 * Adapted from TamaMe (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet.h, Apache-2.0).
 * Removed versus TamaMe: muse_pet_conversation_completed() (design §4.6:
 * conversation rewards are cancelled) and all SFX actions. The worker is the
 * only writer of pet state; UI threads only send actions and read snapshots
 * and feedback (design §3.1).
 */
#pragma once

#include "muse_pet_model.h"
#include <stdbool.h>
#include <stdint.h>

/* Feedback for one settled action. `fortune` is the current fortune index
 * (meaningful for PET_FORTUNE results; otherwise the last drawn index). */
typedef struct {
    muse_pet_action_t action;
    muse_pet_result_t result;
    uint32_t fortune;
} muse_pet_feedback_t;

/*
 * Start the worker (idempotent). Never blocks and never aborts the app:
 * on failure it only logs, and muse_pet_request() then returns false so
 * phase-2 UI can show "宠物未就绪" (design §2.5, §5.3). Call it from
 * muse_app_run() immediately after muse_time_start(), before UI/voice start.
 */
void muse_pet_start(void);

/* True once the worker has loaded (or created) state and published a snapshot. */
bool muse_pet_ready(void);

/* True when the worker stopped because the stored blob was invalid. This is
 * distinct from "not ready": phase-2 UI shows "互动数据损坏" for this case
 * and "宠物未就绪" for the not-ready case (design §4.4). */
bool muse_pet_storage_error(void);

/*
 * Queue an action. Returns false when the worker is not ready, when the
 * command queue is full (8), or when the worker never started. Never blocks:
 * in particular, never call this from a context that cannot tolerate a
 * failure return and then wait -- LVGL callbacks must show "宠物未就绪"
 * instead of waiting (design §6.2).
 */
bool muse_pet_request(muse_pet_action_t action);

/* Start a game round with its already-decided outcome: >0 win (+5 coins),
 * <0 loss (-5), 0 draw (no change). Returns false when the worker is not
 * ready; PET_POOR still comes back through the feedback queue when the
 * balance is below the 5-coin stake. */
bool muse_pet_request_game(int outcome);

/* Copy the latest published snapshot. Returns false when not ready. */
bool muse_pet_snapshot(muse_pet_model_t *out);

/* Pop one feedback item without blocking. Returns false when empty. */
bool muse_pet_feedback(muse_pet_feedback_t *out);

/*
 * Push a feedback item back to the front of the queue (console use only:
 * the console's wait-for-result loop must not swallow feedback meant for
 * another consumer). Returns false when the queue is full; in that case the
 * item is lost and the caller must log it.
 */
bool muse_pet_feedback_requeue(const muse_pet_feedback_t *fb);

/*
 * Erase the stored blob directly. Only for the Test console's ">pet.reset"
 * fallback when the worker is not running (e.g. it died on a corrupt blob):
 * after erasing, a reboot rebuilds fresh state. When the worker IS running,
 * use PET_RESET through the queue instead -- never erase behind a live worker.
 */
bool muse_pet_erase_storage(void);

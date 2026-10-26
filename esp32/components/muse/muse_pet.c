/* Public-edition staging: local UI additions and/or privacy review; see source manifest. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet.c -- 1.5a pet worker.
 *
 * Adapted from TamaMe (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet.c, Apache-2.0).
 * Deleted versus TamaMe, each for a design reason (see PHASE1-NOTES.md):
 *  - clock_start() / self-started SNTP: this project already does RTC adopt
 *    + SNTP in muse_time_start() (design §4.5.2). The worker uses time().
 *  - muse_mem.h: not needed here; the worker allocates no heap buffers.
 *  - Uncalibrated-decay tracking (36-second quanta, reserved seconds):
 *    1.5a has no hourly decay, so there is nothing to accrue while the clock
 *    is uncalibrated; the model simply does nothing until calibrated.
 *  - PET_CONVERSATION / SFX: conversation rewards are cancelled (§4.6) and
 *    1.5a adds no pet sound effects (§5.3).
 *  - TamaMe's "persist at most every 30 minutes" tick rule: 1.5a persists on
 *    every state change instead, including the initial day's living cost,
 *    because design §4.4 requires "先落盘再公布成功" and the living-cost
 *    notice must only be announced after it is persisted.
 *
 * Threading contract (design §3.1, §6.2): this task is the ONLY writer of
 * pet state. UI threads send actions and read the published snapshot and
 * the feedback queue; they never touch the worker's private state.
 */
#include "muse_pet.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>
#include <time.h>

#define MUSE_PET_NVS_NAMESPACE "pet_pub_stats" /* design §4.4, TamaMe name */
#define MUSE_PET_NVS_KEY "state"
#define MUSE_PET_QUEUE_LEN 8    /* design §6.2, TamaMe shape */
#define MUSE_PET_TICK_MS 30000  /* 30 s round: day rollover check, no polling */
#define MUSE_PET_STACK 4096     /* TamaMe starting point; see PHASE1-NOTES.md §6 */
#define MUSE_PET_PRIORITY 2     /* below input (6) and serial (5) in 1.4d */

static const char *TAG = "muse_pet";

/* One queued command. `arg` is the game outcome (-1 lose / 0 draw / +1 win)
 * for PET_GAME and 0 for everything else (1.5a settles by result). */
typedef struct {
    muse_pet_action_t action;
    int32_t arg;
} pet_cmd_t;

/* The 1.4 / 1.5a v2 layout, kept verbatim so an upgrade can carry coins and
 * day counters across instead of disabling rewards (see migrate_v2). */
typedef struct {
    uint32_t version;
    uint32_t coins;
    int32_t touch_day;
    uint32_t daily_touch_coins;
    int32_t feed_day;
    uint32_t daily_feed_coins;
    int32_t fortune_day;
    uint32_t fortune;
    int32_t decay_day;
    uint32_t decay_notice;
} pet_state_v2_t;

_Static_assert(sizeof(pet_state_v2_t) == 40, "v2 blob layout changed");

static QueueHandle_t s_commands;
static QueueHandle_t s_feedback;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_pet_model_t s_visible;
static bool s_ready;
static bool s_started;
static bool s_storage_error;

static void publish(const muse_pet_model_t *s)
{
    portENTER_CRITICAL(&s_lock);
    s_visible = *s;
    s_ready = true;
    portEXIT_CRITICAL(&s_lock);
}

static bool store(nvs_handle_t h, const muse_pet_model_t *s)
{
    return nvs_set_blob(h, MUSE_PET_NVS_KEY, s, sizeof(*s)) == ESP_OK &&
           nvs_commit(h) == ESP_OK;
}

/* Read the version word of a possibly-v2 blob without type-punning the
 * pointer (the toolchain builds with -Werror=strict-aliasing). */
static bool blob_version_is_v2(const muse_pet_model_t *blob)
{
    uint32_t version = 0;
    memcpy(&version, blob, sizeof(version));
    return version == 2u;
}

/* v2 kept one "already fed today" coin counter; v3 keeps a mask with one bit
 * per food (user decision 2026-10-25: each food pays its own 10). Which food
 * the old counter covered is unknown, so the three foods start unclaimed --
 * on the upgrade day that can pay up to 30 coins one extra time. Coins and the
 * touch / fortune / decay day counters carry over untouched. */
static void migrate_v2(const pet_state_v2_t *old, muse_pet_model_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = MUSE_PET_VERSION;
    out->coins = old->coins > MUSE_PET_COINS_MAX ? MUSE_PET_COINS_MAX : old->coins;
    out->touch_day = old->touch_day;
    out->daily_touch_coins = old->daily_touch_coins > MUSE_PET_TOUCH_DAILY_MAX
                                 ? MUSE_PET_TOUCH_DAILY_MAX
                                 : old->daily_touch_coins;
    out->feed_day = old->feed_day;
    out->feed_mask = 0;
    out->fortune_day = old->fortune_day;
    out->fortune = old->fortune < MUSE_PET_FORTUNES ? old->fortune : 0;
    out->decay_day = old->decay_day;
    out->decay_notice = old->decay_notice > MUSE_PET_COINS_MAX
                            ? MUSE_PET_COINS_MAX
                            : old->decay_notice;
}

static void worker(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open(MUSE_PET_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable");
        portENTER_CRITICAL(&s_lock);
        s_storage_error = true;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelete(NULL);
        return;
    }
    muse_pet_model_t state;
    size_t bytes = sizeof(state);
    esp_err_t err = nvs_get_blob(h, MUSE_PET_NVS_KEY, &state, &bytes);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        muse_pet_model_init(&state);
        if (!store(h, &state)) {
            ESP_LOGE(TAG, "initial save failed");
            nvs_close(h);
            vTaskDelete(NULL);
            return;
        }
    } else if (err == ESP_OK && bytes == sizeof(pet_state_v2_t) &&
               blob_version_is_v2(&state)) {
        /* 1.4 / earlier 1.5a state: carry it over rather than disabling
         * rewards (user decision 2026-10-25). Persist the migrated form before
         * publishing, so a later read never sees v2 again. */
        pet_state_v2_t legacy;
        memcpy(&legacy, &state, sizeof(legacy));
        migrate_v2(&legacy, &state);
        if (!store(h, &state)) {
            ESP_LOGE(TAG, "migration save failed");
            nvs_close(h);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "pet state migrated v2 -> v%d", (int)MUSE_PET_VERSION);
    } else if (err != ESP_OK || bytes != sizeof(state) ||
               !muse_pet_model_valid(&state)) {
        /* Never silently reset (design §4.4): rewards stay disabled until a
         * Test console ">pet.reset" clears the blob and the device reboots. */
        ESP_LOGE(TAG, "invalid storage; rewards disabled");
        portENTER_CRITICAL(&s_lock);
        s_storage_error = true;
        portEXIT_CRITICAL(&s_lock);
        nvs_close(h);
        vTaskDelete(NULL);
        return;
    }

    /* Settle the current day's living cost before the first snapshot, and
     * persist it before publishing: the notice must only be announced after
     * it is on disk (design §4.4). A failed store here keeps the loaded
     * state; the next tick retries the decay. */
    muse_pet_model_t settled = state;
    muse_pet_model_decay(&settled, time(NULL));
    if (memcmp(&settled, &state, sizeof(state)) != 0) {
        if (store(h, &settled)) {
            state = settled;
        } else {
            ESP_LOGE(TAG, "initial decay save failed; retrying on next tick");
        }
    }
    publish(&state);

    for (;;) {
        pet_cmd_t cmd = {PET_TOUCH, 0};  /* overwritten by the receive below */
        bool command = xQueueReceive(s_commands, &cmd,
                                     pdMS_TO_TICKS(MUSE_PET_TICK_MS)) == pdTRUE;
        muse_pet_action_t action = cmd.action;
        muse_pet_model_t candidate = state;
        int64_t now = time(NULL);
        muse_pet_result_t result;
        if (command) {
            result = muse_pet_model_apply(&candidate, action, now, esp_random(),
                                          cmd.arg);
        } else {
            /* Tick: day rollover / living cost only. No action, no feedback. */
            muse_pet_model_decay(&candidate, now);
            result = PET_OK;
        }
        bool changed = memcmp(&candidate, &state, sizeof(state)) != 0;
        if (changed) {
            /* Persist before publishing success (design §4.4). */
            if (!store(h, &candidate)) {
                if (command) {
                    result = PET_STORAGE;
                }
                candidate = state; /* revert: RAM must not run ahead of disk */
                changed = false;
            }
        }
        if (result != PET_STORAGE) {
            state = candidate;
            if (changed) {
                publish(&state);
            }
        }
        if (command && action != PET_DECAY_ACK) {
            /* Feedback is only a UI notification -- the snapshot is the
             * authority -- so a full queue must never stall this task.
             * 1.5b: send with a 100 ms timeout and drop on timeout
             * instead of TamaMe's portMAX_DELAY (the UI tick drains every
             * frame, so a drop means the consumer stopped, not that the
             * state is wrong; the next snapshot refresh still lands). */
            muse_pet_feedback_t feedback = {action, result, state.fortune};
            if (xQueueSend(s_feedback, &feedback, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "feedback dropped (queue full)");
            }
        }
    }
}

void muse_pet_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    s_commands = xQueueCreate(MUSE_PET_QUEUE_LEN, sizeof(pet_cmd_t));
    s_feedback = xQueueCreate(MUSE_PET_QUEUE_LEN, sizeof(muse_pet_feedback_t));
    /*
     * Internal RAM stack, on purpose: NVS commits disable cache, so a PSRAM
     * stack is unsafe here (TamaMe comment, design §6.2). xTaskCreate with
     * no PSRAM-stack option already lands the stack in internal RAM; do NOT
     * "optimise" this into a PSRAM stack to save internal RAM.
     */
    if (!s_commands || !s_feedback ||
        xTaskCreate(worker, "muse_pet", MUSE_PET_STACK, NULL,
                    MUSE_PET_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker unavailable");
    }
}

bool muse_pet_ready(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

bool muse_pet_storage_error(void)
{
    portENTER_CRITICAL(&s_lock);
    bool bad = s_storage_error;
    portEXIT_CRITICAL(&s_lock);
    return bad;
}

static bool request_cmd(muse_pet_action_t action, int32_t arg)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_lock);
    pet_cmd_t cmd = {action, arg};
    return ready && s_commands && xQueueSend(s_commands, &cmd, 0) == pdTRUE;
}

bool muse_pet_request(muse_pet_action_t action)
{
    /* 1.5b: PET_DECAY_ACK takes a day (see muse_pet_decay_ack()). The
     * bare request would carry arg 0, which never matches a real
     * decay_day, so refuse it rather than silently ACKing nothing. */
    if (action == PET_DECAY_ACK) {
        return false;
    }
    return request_cmd(action, 0);
}

bool muse_pet_decay_ack(int32_t day)
{
    return request_cmd(PET_DECAY_ACK, day);
}

bool muse_pet_request_game(int outcome)
{
    /* The UI decides win/lose/draw at click time (design §5.2) and tells the
     * worker, which settles 5/-5/0. Anything that is not a win or a loss is a
     * draw. */
    int32_t arg = outcome > 0 ? 1 : (outcome < 0 ? -1 : 0);
    return request_cmd(PET_GAME, arg);
}

bool muse_pet_snapshot(muse_pet_model_t *out)
{
    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    *out = s_visible;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

bool muse_pet_feedback(muse_pet_feedback_t *out)
{
    return out != NULL && s_feedback &&
           xQueueReceive(s_feedback, out, 0) == pdTRUE;
}

bool muse_pet_feedback_requeue(const muse_pet_feedback_t *fb)
{
    return fb != NULL && s_feedback &&
           xQueueSendToFront(s_feedback, fb, 0) == pdTRUE;
}

bool muse_pet_erase_storage(void)
{
    nvs_handle_t h;
    if (nvs_open(MUSE_PET_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(h, MUSE_PET_NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

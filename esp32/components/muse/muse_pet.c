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
        muse_pet_action_t action;
        bool command = xQueueReceive(s_commands, &action,
                                     pdMS_TO_TICKS(MUSE_PET_TICK_MS)) == pdTRUE;
        muse_pet_model_t candidate = state;
        int64_t now = time(NULL);
        muse_pet_result_t result;
        if (command) {
            result = muse_pet_model_apply(&candidate, action, now, esp_random());
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
            /* Feedback queue is drained by the consumer (phase-2 UI tick, or
             * the console wait loop). TamaMe's portMAX_DELAY is kept: a full
             * feedback queue stalls this task, which is why the consumer
             * MUST drain it every tick (see PHASE1-NOTES.md §6). */
            muse_pet_feedback_t feedback = {action, result, state.fortune};
            xQueueSend(s_feedback, &feedback, portMAX_DELAY);
        }
    }
}

void muse_pet_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    s_commands = xQueueCreate(MUSE_PET_QUEUE_LEN, sizeof(muse_pet_action_t));
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

bool muse_pet_request(muse_pet_action_t action)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_lock);
    return ready && s_commands &&
           xQueueSend(s_commands, &action, 0) == pdTRUE;
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

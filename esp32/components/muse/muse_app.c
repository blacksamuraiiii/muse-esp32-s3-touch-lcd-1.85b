/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_board.h"
#include "muse_ble.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_pet.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_rtc.h"
#include "muse_telemetry.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_VIDEO_WIFI
#include "muse_video_wifi.h"
#endif

static const char *TAG = "muse";

/* Applies saved settings to hardware; runs in whichever task changed them. */
static void on_setting(muse_setting_t what)
{
    switch (what) {
    case MUSE_SETTING_VOLUME:
        muse_audio_set_volume(muse_settings_volume());
        break;
    case MUSE_SETTING_MIC_GAIN:
        muse_audio_set_mic_gain(muse_settings_mic_gain());
        break;
    case MUSE_SETTING_WIFI:
        muse_wifi_apply();
        break;
    case MUSE_SETTING_BLE:
        muse_ble_apply();
        break;
    case MUSE_SETTING_HATCH:
        muse_hatch_config_changed();
        break;
    case MUSE_SETTING_PROXY:
        /* The chat session re-reads the host on its next connect; Link applies
         * the account API base from its own loop. Dropping the session now
         * makes that reconnect happen at once rather than on the next idle
         * timeout. */
        muse_hatch_config_changed();
        break;
    case MUSE_SETTING_ALARM:
        /* The input task owns the RTC arm: a new time needs a new arm, and a
         * switch-off has to clear the chip's flag. */
        muse_input_alarm_sync();
        break;
    default:
        break;   /* brightness, sleep and the speaker are polled where they're used */
    }
}

const muse_board_t *muse_board;

/* One line per boot stage. The audio init failure is a heap-shape problem, not
 * a codec one: what decides it is the largest internal block at ~6 s, and that
 * number changes with Wi-Fi state. Without these lines the only way to see it
 * is to reproduce a failed boot. */
static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "heap after %-10s internal free %6u largest %6u, dma largest %6u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

void muse_app_run(const muse_board_t *board)
{
    muse_board = board;
    ESP_LOGI(TAG, "board: %s", board->name);
    log_heap("app-start");
    ESP_ERROR_CHECK(board->init());
    log_heap("board");
    ESP_ERROR_CHECK(muse_settings_init());
    log_heap("settings");
    muse_settings_set_listener(on_setting);
    muse_state_init();
    log_heap("state");
    muse_battery_init();
    log_heap("battery");
    /* Before the UI: adopts the RTC's time when the system clock is unset, so
     * the first frame already shows a real clock rather than a placeholder. The
     * SNTP half waits for Wi-Fi on its own. */
#if CONFIG_MUSE_BUILD_TEST
    /* Say what this image is, in the log a night's recording is read against:
     * Test carries the telemetry sampler and the bench console commands, a
     * Release image has neither (docs/20261023-待办.md §1). */
    ESP_LOGW(TAG, "TEST BUILD: night-telemetry sampler and bench console commands are compiled "
                  "in; this image is not for the release channel");
#endif
    muse_time_start();
    log_heap("time");
    /* 1.5a pet worker: after the clock is adopted, before UI/voice start.
     * muse_pet_start() never blocks and never aborts the app: on failure it
     * only logs, and muse_pet_request() then returns false. */
    muse_pet_start();
    log_heap("pet");
#if CONFIG_MUSE_TELEMETRY
    /* Test only: mounts the FAT partition and starts the night recorder.
     * After the clock (every line carries a timestamp) and before the input
     * task, which writes the boot event. */
    muse_telemetry_start();
    log_heap("telemetry");
#endif
    muse_state_set_caption("WAKING UP...");
    /* Before the UI: the touch boards' settings show Phone camera only when
     * a transport is there as they're built. Its server waits for Wi-Fi. */
#if CONFIG_MUSE_VIDEO_WIFI
    muse_video_wifi_start();
    log_heap("video_wifi");
#endif
    ESP_ERROR_CHECK(muse_ui_start());
    ESP_LOGI(TAG, "UI built: free internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    log_heap("ui");

    QueueHandle_t q = xQueueCreate(16, sizeof(muse_input_event_t));
    ESP_ERROR_CHECK(muse_input_start(q));
    log_heap("input");

    /* Let the boot animation (flame ignites, eyes open) play out. */
    vTaskDelay(pdMS_TO_TICKS(1400));
    if (!board->audio_init && !board->mic_init) {
        ESP_LOGI(TAG, "display-only board: no microphone or speaker");
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");
    } else if (muse_voice_start(q) != ESP_OK) {
        ESP_LOGE(TAG, "voice pipeline unavailable");
    } else {
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");   /* the button icons say how to talk */
    }
    log_heap("voice");

    muse_hatch_start();
    /* Home Link owns the radios; these just hand it the saved settings. */
    muse_wifi_apply();
    muse_ble_apply();
    ESP_LOGI(TAG, "ready: free heap %u internal, %u psram",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/*
 * Bench-only power attribution sweeps (see muse_powcfg.h).
 */

#include "muse_powcfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_input.h"
#include "muse_state.h"

#include "muse_audio.h"
#include "muse_board.h"
#if CONFIG_MUSE_TELEMETRY
#include "muse_telemetry.h"
#endif

static const char *TAG = "muse_powcfg";

static esp_pm_lock_handle_t s_cpu_lock;    /* CPU_FREQ_MAX */
static esp_pm_lock_handle_t s_sleep_lock;  /* NO_LIGHT_SLEEP */
static bool s_locks_ready;

static esp_err_t ensure_locks(void)
{
    if (s_locks_ready) {
        return ESP_OK;
    }
    esp_err_t e = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "powcfg_cpu", &s_cpu_lock);
    if (e) {
        return e;
    }
    e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "powcfg_sleep", &s_sleep_lock);
    if (e) {
        return e;
    }
    s_locks_ready = true;
    return ESP_OK;
}

static void mark(const char *name)
{
#if CONFIG_MUSE_TELEMETRY
    muse_telemetry_event("powcfg", name);
#endif
}

/* Resting state: exactly what the device does with the screen off. */
static void resting(void)
{
    if (muse_board->set_brightness) {
        muse_board->set_brightness(0);
    }
    if (muse_board->panel_sleep) {
        muse_board->panel_sleep(true);
    }
    muse_audio_power(false);
    if (s_locks_ready) {
        esp_pm_lock_release(s_cpu_lock);
        esp_pm_lock_release(s_sleep_lock);
    }
}

/* Battery sweep: the board is unplugged, so no command can arrive while it
 * runs. The list is fixed and each step holds long enough for the 60 s
 * recorder to take three samples. */
#define POWCFG_STEP_S 180
static volatile bool s_auto_run;
static volatile bool s_auto_stop;

static void auto_task(void *arg)
{
    (void)arg;
    static const char *seq[] = { "base", "panel_awake", "codec_open", "base" };
    muse_input_bench_hold(true);
    char buf[56];
    /* A charging pack masks the gauge, so wait for the cable to come out
     * instead of relying on the operator to unplug at the right second. */
    for (int s = 0; s < 600 && !s_auto_stop && !muse_state_on_battery(); s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!muse_state_on_battery() && !s_auto_stop) {
        mark("auto_no_battery");
        s_auto_run = false;
        muse_input_bench_hold(false);
        vTaskDelete(NULL);
        return;
    }
    mark("auto_begin");
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]) && !s_auto_stop; i++) {
        if (!muse_state_on_battery()) { mark("auto_usb_present"); }
        muse_powcfg_apply(seq[i], buf, sizeof(buf));
        for (int s = 0; s < POWCFG_STEP_S && !s_auto_stop; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    muse_input_bench_hold(false);
    s_auto_run = false;
    mark(s_auto_stop ? "auto_aborted" : "auto_done");
    ESP_LOGI(TAG, "auto sweep %s", s_auto_stop ? "aborted" : "done");
    s_auto_stop = false;
    vTaskDelete(NULL);
}

static const char *auto_start(void)
{
    if (s_auto_run) {
        return "auto: already running";
    }
    s_auto_run = true;
    s_auto_stop = false;
    if (xTaskCreate(auto_task, "powcfg", 4096, NULL, 4, NULL) != pdPASS) {
        s_auto_run = false;
        return "error: task create";
    }
    return "auto: waits for battery, 4x180s";
}

esp_err_t muse_powcfg_apply(const char *name, char *out, size_t capacity)
{
    if (!name || !out || !capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!name[0]) {
        snprintf(out, capacity, "names: base nolightsleep cpu240 panel_awake codec_open auto stop");
        return ESP_OK;
    }
    if (!strcmp(name, "auto")) {
        snprintf(out, capacity, "%s", auto_start());
        return ESP_OK;
    }
    if (!strcmp(name, "stop")) {
        s_auto_stop = true;
        snprintf(out, capacity, "stop: sweep will end");
        return ESP_OK;
    }
    esp_err_t e = ensure_locks();
    if (e) {
        snprintf(out, capacity, "error: pm lock create %d", e);
        return e;
    }
    resting();
    const char *detail = name;

    if (!strcmp(name, "base")) {
        detail = "base: SLPIN, bl0, codecs closed";
    } else if (!strcmp(name, "nolightsleep")) {
        e = esp_pm_lock_acquire(s_sleep_lock);
        detail = "nolightsleep: +NO_LIGHT_SLEEP";
    } else if (!strcmp(name, "cpu240")) {
        e = esp_pm_lock_acquire(s_cpu_lock);
        detail = "cpu240: +CPU_FREQ_MAX";
    } else if (!strcmp(name, "panel_awake")) {
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(false);
        }
        if (muse_board->set_brightness) {
            muse_board->set_brightness(0);
        }
        detail = "panel_awake: SLPOUT, bl0";
    } else if (!strcmp(name, "codec_rail")) {
        if (muse_board->codec_power) {
            muse_board->codec_power(true);
        } else {
            detail = "codec_rail: no switchable rail";
        }
    } else if (!strcmp(name, "codec_open")) {
        if (muse_board->codec_power) {
            muse_board->codec_power(true);
        }
        muse_audio_power(true);
        e = ESP_OK;
        detail = "codec_open: codecs+I2S on";
    } else {
        snprintf(out, capacity, "error: unknown name %s", name);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "%s (err %d)", detail, e);
    mark(name);
    snprintf(out, capacity, "%s", detail);
    return e;
}

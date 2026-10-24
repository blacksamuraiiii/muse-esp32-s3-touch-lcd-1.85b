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

/*
 * The night recorder. See muse_telemetry.h for the format and why it exists.
 *
 * Design notes worth keeping:
 *
 *  - The file is opened, written and closed for every line. That costs a FAT
 *    directory update per minute (a few tens of milliseconds) and buys the one
 *    property that matters here: a device that dies mid-night - flat battery,
 *    a crash, a hard power-off - loses at most the line being written, never
 *    the night. An open FILE* would buffer the last records in RAM and lose
 *    them exactly when the recording is most interesting.
 *
 *  - Nothing here samples anything expensive. The battery numbers come from
 *    muse_battery's accumulator (which the input task already feeds), the Wi-Fi
 *    power-save mode is one getter, and the rest are state bits. The 60 s
 *    cadence is set by the question, not by a budget: a state that changes and
 *    changes back inside a minute is not what an overnight drain is made of.
 *
 *  - The sampler is a normal task with a 60 s delay, so it does not fight the
 *    power management: light sleep between samples is untouched, and the flash
 *    write takes its own PM lock for the ~50 ms it needs.
 */

#include "muse_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"

#if CONFIG_MUSE_TELEMETRY

#include "esp_wifi.h"

#include "muse_battery.h"
#include "muse_rtc.h"
#include "muse_state.h"

static const char *TAG = "muse_telem";

/* The Test partition table's FAT partition, and the one file in it. */
#define TELEM_PARTITION "telemetry"
#define TELEM_BASE "/telemetry"
#define TELEM_PATH TELEM_BASE "/muse.csv"

#define TELEM_PERIOD_MS (60 * 1000)
#define TELEM_TASK_STACK 4096
#define TELEM_TASK_PRIO 2

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_active;
static uint32_t s_last_sleeps;
static bool s_have_last;

/* "2026-10-23T13:05:00Z", or "unknown" before the clock has a real time. The
 * recorder is useless without one, but a boot with no RTC and no network still
 * has to write something rather than nothing. */
static const char *stamp(char *buf, size_t len)
{
    time_t now;
    if (!muse_rtc_now(&now)) {
        snprintf(buf, len, "unknown");
        return buf;
    }
    struct tm utc;
    gmtime_r(&now, &utc);
    /* % 10000 on the year is not cosmetic: without it gcc's format-truncation
     * analysis has to assume a full int and rejects the 32-byte buffer, which
     * is -Werror here. Four digits is all a timestamp needs. */
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", (utc.tm_year + 1900) % 10000,
             utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buf;
}

/* One line, appended, closed. Returns false when the write failed, which is
 * worth a log line but never worth stopping the recorder. */
static bool append(const char *line)
{
    FILE *f = fopen(TELEM_PATH, "a");
    if (f == NULL) {
        return false;
    }
    const bool ok = fputs(line, f) >= 0 && fputc('\n', f) != EOF;
    const bool closed = fclose(f) == 0;
    return ok && closed;
}

/* The column names, written once when the file is created so the CSV explains
 * itself in a spreadsheet a year from now. */
static void write_header_if_new(void)
{
    FILE *f = fopen(TELEM_PATH, "r");
    if (f != NULL) {
        const bool empty = fgetc(f) == EOF;
        fclose(f);
        if (!empty) {
            return;
        }
    }
    (void)append("# muse telemetry: P = periodic sample, E = event. "
                 "rest and lightsleep_pct are cumulative % since the battery "
                 "measurement started; wakes_delta is wakes in this interval.");
    (void)append("#P,utc,pct,avg_mA,volt_mV,temp_C,ps,asleep,rest,alarm_armed,"
                 "lightsleep_pct,wakes_delta");
    (void)append("#E,utc,what,detail");
}

/* Wi-Fi power save as the number the CSV carries: 0 none, 1 min modem, 2 max
 * modem, -1 unknown (Wi-Fi not started, or not this board's radio). */
static int wifi_ps(void)
{
    wifi_ps_type_t ps;
    if (esp_wifi_get_ps(&ps) != ESP_OK) {
        return -1;
    }
    switch (ps) {
    case WIFI_PS_NONE: return 0;
    case WIFI_PS_MIN_MODEM: return 1;
    case WIFI_PS_MAX_MODEM: return 2;
    default: return -1;
    }
}

static void sample(void)
{
    char utc[32];
    stamp(utc, sizeof(utc));

    const muse_power_t p = muse_state_power();
    muse_battery_t b;
    memset(&b, 0, sizeof(b));
    muse_battery_read(&b);

    /* Wakes since the previous line: the difference is what the interval
     * actually contained, while b.sleeps is the whole run. */
    const uint32_t wakes_delta = s_have_last && b.sleeps >= s_last_sleeps
                                     ? b.sleeps - s_last_sleeps
                                     : 0;
    s_last_sleeps = b.sleeps;
    s_have_last = true;

    /* muse_battery keeps per-mille accumulators; /10 turns them into the
     * percent the column promises. -1 (unknown) stays -1. */
    const int rest = b.resting_pm < 0 ? -1 : b.resting_pm / 10;
    const int lightsleep = b.slept_pm < 0 ? -1 : b.slept_pm / 10;

    /* The pack temperature as "31.2", keeping the sign of a tenth below zero
     * (-0.3 must not print as 0.3). */
    const int t10 = p.battery_temp_c10;
    const int t_whole = t10 / 10;
    const char *t_sign = (t10 < 0 && t_whole == 0) ? "-" : "";

    char line[192];
    snprintf(line, sizeof(line),
             "P,%s,%d,%d,%d,%s%d.%d,%d,%d,%d,%d,%d,%lu",
             utc, p.battery_pct, p.battery_ma, p.battery_mv, t_sign, t_whole,
             abs(t10 % 10), wifi_ps(), muse_state_asleep() ? 1 : 0, rest,
             muse_rtc_wake_armed() ? 1 : 0, lightsleep, (unsigned long)wakes_delta);
    if (!append(line)) {
        ESP_LOGW(TAG, "append failed: the FAT partition may be full");
    }
}

static void sampler_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TELEM_PERIOD_MS));
        sample();
    }
}

void muse_telemetry_start(void)
{
    if (s_active) {
        return;
    }
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 2,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(TELEM_BASE, TELEM_PARTITION, &cfg, &s_wl);
    if (err != ESP_OK) {
        /* The usual cause is a Release partition table: no "telemetry" label.
         * Say which, because the fix is a reflash, not a reboot. */
        ESP_LOGE(TAG, "no %s partition (%s): telemetry off. This firmware needs the "
                      "Test partition table.", TELEM_PARTITION, esp_err_to_name(err));
        return;
    }
    write_header_if_new();
    if (xTaskCreate(sampler_task, "muse_telem", TELEM_TASK_STACK, NULL, TELEM_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "no task: telemetry off");
        esp_vfs_fat_spiflash_unmount_rw_wl(TELEM_BASE, s_wl);
        s_wl = WL_INVALID_HANDLE;
        return;
    }
    s_active = true;
    ESP_LOGI(TAG, "recording to %s every %d s", TELEM_PATH, TELEM_PERIOD_MS / 1000);
}

bool muse_telemetry_active(void)
{
    return s_active;
}

void muse_telemetry_event(const char *what, const char *detail)
{
    if (!s_active || what == NULL) {
        return;
    }
    char utc[32];
    stamp(utc, sizeof(utc));
    char line[160];
    snprintf(line, sizeof(line), "E,%s,%s,%s", utc, what, detail ? detail : "");
    (void)append(line);
}

void muse_telemetry_dump(void)
{
    if (!s_active) {
        printf("@telemetry.dump error not-recording\n");
        return;
    }
    FILE *f = fopen(TELEM_PATH, "r");
    if (f == NULL) {
        printf("@telemetry.dump error no-file\n");
        return;
    }
    printf("@telemetry.dump begin\n");
    char line[256];
    size_t n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        fputs(line, stdout);
        n++;
    }
    fclose(f);
    printf("@telemetry.dump end %u\n", (unsigned)n);
}

void muse_telemetry_stats(void)
{
    if (!s_active) {
        printf("@telemetry.stats error not-recording\n");
        return;
    }
    FILE *f = fopen(TELEM_PATH, "r");
    if (f == NULL) {
        printf("@telemetry.stats error no-file\n");
        return;
    }
    unsigned samples = 0, events = 0;
    long sum_ma = 0;
    int min_ma = 0, max_ma = 0, have_ma = 0;
    long sum_rest = 0, sum_sleep = 0;
    unsigned rest_n = 0, sleep_n = 0;
    unsigned ps_dwell[3] = { 0, 0, 0 };
    unsigned long wakes = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#') {
            continue;
        }
        if (line[0] == 'E') {
            events++;
            continue;
        }
        if (line[0] != 'P') {
            continue;
        }
        /* P,utc,pct,avg_mA,volt_mV,temp_C,ps,asleep,rest,alarm_armed,lightsleep_pct,wakes_delta */
        char utc[32];
        int pct = 0, ma = 0, mv = 0, t_int = 0, t_frac = 0, ps = -1, asleep = 0, rest = -1,
            armed = 0, sleep = -1;
        unsigned long wd = 0;
        if (sscanf(line, "P,%31[^,],%d,%d,%d,%d.%d,%d,%d,%d,%d,%d,%lu", utc, &pct, &ma, &mv,
                   &t_int, &t_frac, &ps, &asleep, &rest, &armed, &sleep, &wd) != 12) {
            continue;
        }
        samples++;
        if (ps >= 0 && ps <= 2) {
            ps_dwell[ps]++;
        }
        if (ma != 0) {   /* 0 is "the gauge had nothing to say", not 0 mA */
            sum_ma += ma;
            min_ma = have_ma && min_ma < ma ? min_ma : ma;
            max_ma = have_ma && max_ma > ma ? max_ma : ma;
            have_ma++;
        }
        if (rest >= 0) {
            sum_rest += rest;
            rest_n++;
        }
        if (sleep >= 0) {
            sum_sleep += sleep;
            sleep_n++;
        }
        wakes += wd;
    }
    fclose(f);

    const long avg_ma = have_ma ? sum_ma / have_ma : 0;
    const long rest_avg = rest_n ? sum_rest / (long)rest_n : -1;
    const long sleep_avg = sleep_n ? sum_sleep / (long)sleep_n : -1;
    printf("@telemetry.stats samples=%u events=%u ma_min=%d ma_avg=%ld ma_max=%d "
           "rest_pct=%ld lightsleep_pct=%ld wakes=%lu ps_none=%u ps_min=%u ps_max=%u\n",
           samples, events, have_ma ? min_ma : 0, avg_ma, have_ma ? max_ma : 0,
           rest_avg, sleep_avg, wakes, ps_dwell[0], ps_dwell[1], ps_dwell[2]);
}

void muse_telemetry_clear(void)
{
    if (!s_active) {
        printf("@telemetry.clear error not-recording\n");
        return;
    }
    if (remove(TELEM_PATH) != 0) {
        /* Nothing recorded yet is not an error worth a different answer. */
        ESP_LOGW(TAG, "no file to remove");
    }
    write_header_if_new();
    s_have_last = false;
    printf("@telemetry.clear ok\n");
}

#endif /* CONFIG_MUSE_TELEMETRY */

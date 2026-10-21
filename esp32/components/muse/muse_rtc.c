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
 * The clock. Waveshare's pcf85063a component owns the device handle and the
 * I2C transport; this file is the layer a device needs on top of it - UTC in
 * and out, the OS/validity check, the STOP-bracketed write, and the SNTP side
 * that gives the chip something real to keep. See muse_rtc.h for why the
 * component's own time getter is not used for the date.
 */

#include "muse_rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "third_party/pcf85063a/pcf85063a.h"
#include "sdkconfig.h"

#include "muse_wifi.h"

static const char *TAG = "muse_rtc";

/* Older than this means "no real time yet": 2025-01-01 00:00:00 UTC. */
#define SANE_EPOCH 1735689600LL

static pcf85063a_dev_t s_dev;
static bool s_present;
static bool s_valid;
static bool s_sntp_inited;
static bool s_sntp_started;
static volatile bool s_synced;

/* ------------------------------------------------------------ BCD and dates */

static int bcd_to_bin(uint8_t b)
{
    int hi = b >> 4, lo = b & 0x0f;
    if (hi > 9 || lo > 9) {
        return -1;
    }
    return hi * 10 + lo;
}

static uint8_t bin_to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* Howard Hinnant's civil-date algorithm; days relative to 1970-01-01. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t tm_to_unix(const struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900, (unsigned)tm->tm_mon + 1,
                                   (unsigned)tm->tm_mday);
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

/* ------------------------------------------------------------------- the chip */

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len)
{
    return pcf85063a_read_register(&s_dev, reg, buf, (uint8_t)len);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[8];
    if (len == 0 || len > sizeof(tmp) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    return pcf85063a_write_register(&s_dev, tmp, (uint8_t)(len + 1));
}

esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus)
{
    s_present = false;
    s_valid = false;
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* pcf85063a_init() adds the device, writes Control_1 = CAP_SEL (STOP = 0,
     * 12_24 = 0, so 24-hour and running) and applies the vendor's default
     * oscillator offset. It leaves the alarm interrupt alone. */
    esp_err_t err = pcf85063a_init(&s_dev, bus, MUSE_RTC_ADDR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no PCF85063 at 0x%02x (%s): the clock has no backup",
                 MUSE_RTC_ADDR, esp_err_to_name(err));
        return err;
    }
    s_present = true;
    /* A latched alarm flag from a previous session would keep INT low and, if
     * the line is ever armed for deep sleep, wake the chip the instant it
     * sleeps. Clear it now: AF is cleared by writing 0 to it. */
    uint8_t c2 = 0;
    if (rd(PCF85063A_RTC_CTRL_2_ADDR, &c2, 1) == ESP_OK) {
        uint8_t nc2 = c2 & ~(PCF85063A_RTC_CTRL_2_AIE | PCF85063A_RTC_CTRL_2_AF);
        if (nc2 != c2) {
            (void)wr(PCF85063A_RTC_CTRL_2_ADDR, &nc2, 1);
        }
    }
    struct tm tm;
    ESP_LOGI(TAG, "PCF85063 present at 0x%02x, %s", MUSE_RTC_ADDR,
             muse_rtc_get(&tm) ? "holding time" : "no valid time yet (OS set)");
    return ESP_OK;
}

bool muse_rtc_present(void)
{
    return s_present;
}

bool muse_rtc_time_valid(void)
{
    return s_present && s_valid;
}

bool muse_rtc_get(struct tm *out)
{
    if (!s_present || out == NULL) {
        return false;
    }
    /* 04h-0Ah in one transaction: the datasheet requires it so a carry cannot
     * tear the read, and it is the only way to see Seconds.OS in the same
     * snapshot as the counters it invalidates. */
    uint8_t r[7];
    if (rd(PCF85063A_RTC_SECOND_ADDR, r, 7) != ESP_OK) {
        s_valid = false;
        return false;
    }
    if (r[0] & 0x80) {
        /* Oscillator stopped: first power-on, or the backup cell is flat. The
         * counters are frozen at whatever they held and mean nothing. */
        s_valid = false;
        return false;
    }
    int sec = bcd_to_bin(r[0] & 0x7f);
    int min = bcd_to_bin(r[1] & 0x7f);
    int hour = bcd_to_bin(r[2] & 0x3f);
    int day = bcd_to_bin(r[3] & 0x3f);
    int mon = bcd_to_bin(r[5] & 0x1f);
    int year = bcd_to_bin(r[6]);
    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23 ||
        day < 1 || day > 31 || mon < 1 || mon > 12 || year < 0) {
        s_valid = false;
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec = sec;
    out->tm_min = min;
    out->tm_hour = hour;
    out->tm_mday = day;
    out->tm_mon = mon - 1;
    out->tm_year = 100 + year;   /* the chip has no century: 2000 + yy */
    int64_t days = days_from_civil(2000 + year, (unsigned)mon, (unsigned)day);
    out->tm_wday = (int)(((days + 4) % 7 + 7) % 7);   /* 1970-01-01 was a Thursday */
    out->tm_isdst = 0;
    s_valid = true;
    return true;
}

esp_err_t muse_rtc_set(const struct tm *utc)
{
    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (utc == NULL || utc->tm_mon < 0 || utc->tm_mon > 11 || utc->tm_mday < 1 ||
        utc->tm_mday > 31 || utc->tm_hour < 0 || utc->tm_hour > 23 ||
        utc->tm_min < 0 || utc->tm_min > 59 || utc->tm_sec < 0 || utc->tm_sec > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    /* STOP while the seven counters are replaced, then release: the first tick
     * lands 0.5-1.0 s after the release, which is the accuracy the chip offers
     * to a host that sets it this way. */
    uint8_t c1 = 0;
    if (rd(PCF85063A_RTC_CTRL_1_ADDR, &c1, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t stopped = c1 | PCF85063A_RTC_CTRL_1_STOP;
    esp_err_t err = wr(PCF85063A_RTC_CTRL_1_ADDR, &stopped, 1);
    if (err != ESP_OK) {
        return err;
    }
    int64_t days = days_from_civil(utc->tm_year + 1900, (unsigned)utc->tm_mon + 1,
                                   (unsigned)utc->tm_mday);
    uint8_t w[7] = {
        bin_to_bcd(utc->tm_sec),                    /* bit7 = 0 clears OS */
        bin_to_bcd(utc->tm_min),
        bin_to_bcd(utc->tm_hour),
        bin_to_bcd(utc->tm_mday),
        (uint8_t)(((days + 4) % 7 + 7) % 7),        /* Sunday = 0 */
        bin_to_bcd(utc->tm_mon + 1),
        bin_to_bcd((utc->tm_year + 1900) % 100),
    };
    err = wr(PCF85063A_RTC_SECOND_ADDR, w, sizeof(w));
    uint8_t running = c1 & ~PCF85063A_RTC_CTRL_1_STOP;
    esp_err_t rerr = wr(PCF85063A_RTC_CTRL_1_ADDR, &running, 1);
    if (err == ESP_OK) {
        err = rerr;
    }
    if (err == ESP_OK) {
        s_valid = true;
    }
    return err;
}

/* --------------------------------------------------------- system clock bridge */

bool muse_rtc_adopt(void)
{
    if (time(NULL) >= (time_t)SANE_EPOCH) {
        return false;   /* the system clock already has real time */
    }
    struct tm tm;
    if (!muse_rtc_get(&tm)) {
        return false;
    }
    struct timeval tv = { .tv_sec = tm_to_unix(&tm), .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return false;
    }
    ESP_LOGI(TAG, "system time restored from the RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

esp_err_t muse_rtc_push(void)
{
    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }
    time_t now = time(NULL);
    if (now < (time_t)SANE_EPOCH) {
        return ESP_ERR_INVALID_STATE;   /* nothing real to store yet */
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    esp_err_t err = muse_rtc_set(&tm);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t muse_rtc_set_utc_fields(int year, int month, int day,
                                  int hour, int minute, int second)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    struct tm tm = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
    };
    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    struct timeval tv = { .tv_sec = days * 86400 + hour * 3600 + minute * 60 + second,
                          .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    return muse_rtc_set(&tm);
}

/* ------------------------------------------------------------------- SNTP side */
static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
}

static void time_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_sntp_inited && muse_wifi_connected()) {
            /* Only once the default netif has an address: SNTP resolves the
             * server name, and there is nothing to resolve it with before. */
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MUSE_NTP_SERVER);
            cfg.start = false;
            cfg.sync_cb = on_sync;
            esp_err_t err = esp_netif_sntp_init(&cfg);
            if (err == ESP_OK) {
                s_sntp_inited = true;
                ESP_LOGI(TAG, "SNTP configured: %s", CONFIG_MUSE_NTP_SERVER);
            } else {
                ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
            }
        }
        if (s_sntp_inited && !s_sntp_started) {
            if (esp_netif_sntp_start() == ESP_OK) {
                s_sntp_started = true;
            }
        }
        if (s_synced) {
            s_synced = false;
            /* The chip keeps what the network just proved. SNTP polls again on
             * its own schedule and this runs each time, so drift never builds
             * up past one poll interval. */
            if (muse_rtc_present()) {
                (void)muse_rtc_push();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void muse_time_start(void)
{
    setenv("TZ", CONFIG_MUSE_TIMEZONE, 1);
    tzset();
    muse_rtc_adopt();
    static bool started;
    if (started) {
        return;
    }
    started = true;
    if (xTaskCreate(time_task, "muse_time", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no task: the clock will not be corrected from the network");
    }
}

/* ------------------------------------------------------------------- display */

static bool now_or_rtc(time_t *out)
{
    time_t now = time(NULL);
    if (now >= (time_t)SANE_EPOCH) {
        *out = now;
        return true;
    }
    struct tm tm;
    if (!muse_rtc_get(&tm)) {
        return false;
    }
    *out = (time_t)tm_to_unix(&tm);
    return true;
}

bool muse_rtc_clock_str(char *buf, size_t len)
{
    time_t now;
    if (buf == NULL || len < 6 || !now_or_rtc(&now)) {
        return false;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return true;
}

bool muse_rtc_stamp_str(char *buf, size_t len)
{
    time_t now;
    if (buf == NULL || len < 20 || !now_or_rtc(&now)) {
        return false;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d %s",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec, CONFIG_MUSE_TIMEZONE);
    return true;
}

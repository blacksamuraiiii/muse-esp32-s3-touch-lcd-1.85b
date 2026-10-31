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
 * The clock. Waveshare's pcf85063a component owns the device handle, the I2C
 * transport and the BCD datetime conversion; this file is the layer a device
 * needs on top of it - UTC in and out, the OS/validity check, the STOP-bracketed
 * write, and the SNTP side that gives the chip something real to keep. See
 * muse_rtc.h for which parts of the register map are still touched by hand.
 */

#include "muse_rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
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
static bool s_wake_armed;      /* an alarm is pending: fold RTC_INT into the mask */

/* ------------------------------------------------------------ dates and epoch */

/* Howard Hinnant's civil-date algorithm; days relative to 1970-01-01. Still
 * needed on top of the driver: the chip stores a weekday counter, not a date,
 * and the system clock speaks Unix seconds. */
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

/* Sunday = 0, which is what the chip's weekday register counts. */
static uint8_t weekday_of(int year, int month, int day)
{
    const int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    return (uint8_t)(((days + 4) % 7 + 7) % 7);   /* 1970-01-01 was a Thursday */
}

/* The chip's calendar as Unix seconds, for comparing a written value with the
 * one read back. No weekday term: the register is a counter, not a date. */
static int64_t dt_to_unix(const pcf85063a_datetime_t *dt)
{
    return days_from_civil(dt->year, dt->month, dt->day) * 86400 +
           (int64_t)dt->hour * 3600 + (int64_t)dt->min * 60 + dt->sec;
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
    /* Seconds.OS is the one thing the driver's getter cannot report: it masks
     * bit 7 off. The datasheet wants 04h-0Ah read in one transaction so a carry
     * cannot tear the value, and the OS bit lives in that same first byte, so it
     * is read on its own here and the rest through the driver. A second read of
     * 04h costs one byte and can only ever see a *later* state, never an older
     * one, so the OS verdict is conservative in the right direction. */
    uint8_t sec = 0;
    if (rd(PCF85063A_RTC_SECOND_ADDR, &sec, 1) != ESP_OK) {
        s_valid = false;
        return false;
    }
    if (sec & 0x80) {
        /* Oscillator stopped: first power-on, or the backup cell is flat. The
         * counters are frozen at whatever they held and mean nothing. */
        s_valid = false;
        return false;
    }
    pcf85063a_datetime_t dt;
    memset(&dt, 0, sizeof(dt));
    if (pcf85063a_get_time_date(&s_dev, &dt) != ESP_OK) {
        s_valid = false;
        return false;
    }
    /* The chip keeps no century: year is BCD + YEAR_OFFSET (1970). Anything
     * outside this century is a register that came back wrong, not a date. */
    if (dt.year < 2000 || dt.year > 2099 || dt.month < 1 || dt.month > 12 ||
        dt.day < 1 || dt.day > 31 || dt.hour > 23 || dt.min > 59 || dt.sec > 59) {
        s_valid = false;
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec = dt.sec;
    out->tm_min = dt.min;
    out->tm_hour = dt.hour;
    out->tm_mday = dt.day;
    out->tm_mon = dt.month - 1;
    out->tm_year = dt.year - 1900;
    /* Recomputed rather than taken from dt.dotw: the chip's weekday is a plain
     * counter that a STOP-bracketed write can leave a day off, while the date
     * itself is what everything downstream trusts. */
    out->tm_wday = weekday_of(dt.year, dt.month, dt.day);
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
    const int year = utc->tm_year + 1900;
    const pcf85063a_datetime_t dt = {
        .year = (uint16_t)year,
        .month = (uint8_t)(utc->tm_mon + 1),
        .day = (uint8_t)utc->tm_mday,
        .dotw = weekday_of(year, utc->tm_mon + 1, utc->tm_mday),
        .hour = (uint8_t)utc->tm_hour,
        .min = (uint8_t)utc->tm_min,
        .sec = (uint8_t)utc->tm_sec,
    };
    /* STOP while the seven counters are replaced, then release: the first tick
     * lands 0.5-1.0 s after the release, which is the accuracy the chip offers
     * to a host that sets it this way. The driver has no bracketed write of its
     * own, so the two Control_1 accesses stay here - the only register pokes
     * left in this file, and they are the documented way to do it. */
    uint8_t c1 = 0;
    if (rd(PCF85063A_RTC_CTRL_1_ADDR, &c1, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    const uint8_t stopped = c1 | PCF85063A_RTC_CTRL_1_STOP;
    esp_err_t err = wr(PCF85063A_RTC_CTRL_1_ADDR, &stopped, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = pcf85063a_set_time_date(&s_dev, dt);
    const uint8_t running = c1 & (uint8_t)~PCF85063A_RTC_CTRL_1_STOP;
    const esp_err_t rerr = wr(PCF85063A_RTC_CTRL_1_ADDR, &running, 1);
    if (err == ESP_OK) {
        err = rerr;
    }
    if (err == ESP_OK) {
        /* Read the calendar back. The STOP bracket is three transactions, and a
         * write that never landed would otherwise pass for success until the
         * user notices a wrong clock. The reference for this board does the
         * same (bsp_rtc_set_datetime reads it back and rejects anything more
         * than 2 s off, which also absorbs the tick that lands on release). */
        pcf85063a_datetime_t back;
        memset(&back, 0, sizeof(back));
        if (pcf85063a_get_time_date(&s_dev, &back) != ESP_OK) {
            ESP_LOGW(TAG, "RTC written but not readable back");
            err = ESP_FAIL;
        } else {
            const int64_t drift = dt_to_unix(&back) - dt_to_unix(&dt);
            if (drift < 0 || drift > 2) {
                ESP_LOGW(TAG, "RTC write did not stick: wrote %04u-%02u-%02u %02u:%02u:%02u, "
                              "read %04u-%02u-%02u %02u:%02u:%02u",
                         dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec,
                         back.year, back.month, back.day, back.hour, back.min, back.sec);
                err = ESP_ERR_INVALID_STATE;
            }
        }
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

/* The system clock when it has a real time, the chip otherwise: the one answer
 * a caller that has to compare "now" against an HH:MM needs. */
bool muse_rtc_now(time_t *out)
{
    if (out == NULL) {
        return false;
    }
    return now_or_rtc(out);
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

/* ------------------------------------------------------------------ the alarm */
/*
 * A one-shot "next time these three fields come round" alarm: the driver's
 * pcf85063a_set_alarm() compares seconds, minutes and hours and masks day and
 * weekday out, so the chip raises INT the next time HH:MM:SS matches. The
 * caller arms it with the distance to the moment it wants - the input task
 * recomputes that from the settings after every ring, which is what makes it a
 * daily alarm. The chip counts UTC, so the target is converted with gmtime_r().
 *
 * AF is latched by the part: it stays set - and keeps INT low - until it is
 * written back as 0. Arming clears it (pcf85063a_enable_alarm() writes CTRL_2
 * with AF 0), and so does muse_rtc_disarm(), so a fired alarm never holds the
 * line low into the next sleep.
 */
esp_err_t muse_rtc_arm_wakeup_in(uint32_t in_seconds)
{
    if (in_seconds < 2 || in_seconds > 86399) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }
    time_t now;
    if (!now_or_rtc(&now)) {
        return ESP_ERR_INVALID_STATE;   /* no time to count from yet */
    }
    const time_t target = now + (time_t)in_seconds;
    struct tm utc;
    gmtime_r(&target, &utc);
    const pcf85063a_datetime_t dt = {
        .year = (uint16_t)(utc.tm_year + 1900),
        .month = (uint8_t)(utc.tm_mon + 1),
        .day = (uint8_t)utc.tm_mday,
        .dotw = weekday_of(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday),
        .hour = (uint8_t)utc.tm_hour,
        .min = (uint8_t)utc.tm_min,
        .sec = (uint8_t)utc.tm_sec,
    };
    esp_err_t err = pcf85063a_set_alarm(&s_dev, dt);
    if (err == ESP_OK) {
        err = pcf85063a_enable_alarm(&s_dev);   /* AIE=1, AF cleared */
    }
    s_wake_armed = err == ESP_OK;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "alarm armed for %02d:%02d:%02d UTC (%" PRIu32 " s)", utc.tm_hour,
                 utc.tm_min, utc.tm_sec, in_seconds);
    } else {
        ESP_LOGW(TAG, "alarm arm failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* BCD for the alarm registers; the driver keeps its own converter private. */
static uint8_t dec_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/*
 * A weekly arm: the same register block as the daily one, except the weekday
 * register compares too (docs/20261023-待办.md §2.1). With AEN_W clear the chip
 * pulls INT only when the time matches *and* its weekday counter equals the
 * value written here - so a Tuesday alarm armed on Sunday stays quiet on Sunday
 * and Monday, and the device does not have to wake up to find that out. That
 * matters because deep sleep here is woken by ext1 or this one INT line and
 * nothing else (muse_input.c disables the timer wakeup).
 *
 * The weekday written is the target's, in UTC: gmtime_r() of the target instant
 * already carries it, so the caller never translates weekdays itself. The chip's
 * weekday counter (0x08) is checked first and rewritten from the current date
 * when the two disagree - only the time-set path writes it, so a clock that was
 * corrected without a weekday write would otherwise ring on the wrong day.
 */
esp_err_t muse_rtc_arm_wakeup_weekly(uint32_t in_seconds)
{
    if (in_seconds < 2 || in_seconds > 7u * 86400u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }
    time_t now;
    if (!now_or_rtc(&now)) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm now_utc;
    gmtime_r(&now, &now_utc);
    const uint8_t date_wday = weekday_of(now_utc.tm_year + 1900, now_utc.tm_mon + 1,
                                         now_utc.tm_mday);
    uint8_t reg_wday = 0;
    if (rd(PCF85063A_RTC_WDAY_ADDR, &reg_wday, 1) == ESP_OK && (reg_wday & 0x07u) != date_wday) {
        const uint8_t fixed = dec_to_bcd(date_wday);
        if (wr(PCF85063A_RTC_WDAY_ADDR, &fixed, 1) == ESP_OK) {
            ESP_LOGW(TAG, "weekday register %u disagreed with the date (%u): rewritten",
                     (unsigned)(reg_wday & 0x07u), (unsigned)date_wday);
        }
    }

    const time_t target = now + (time_t)in_seconds;
    struct tm utc;
    gmtime_r(&target, &utc);
    const uint8_t payload[5] = {
        dec_to_bcd((uint8_t)utc.tm_sec),
        dec_to_bcd((uint8_t)utc.tm_min),
        dec_to_bcd((uint8_t)utc.tm_hour),
        PCF85063A_RTC_ALARM,                 /* day of the month does not compare */
        dec_to_bcd((uint8_t)utc.tm_wday),    /* the weekday does */
    };
    esp_err_t err = wr(PCF85063A_RTC_SECOND_ALARM, payload, sizeof(payload));
    if (err == ESP_OK) {
        err = pcf85063a_enable_alarm(&s_dev);   /* AIE=1, AF cleared */
    }
    s_wake_armed = err == ESP_OK;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "weekly alarm armed for %02d:%02d:%02d UTC, weekday %u (%" PRIu32 " s)",
                 utc.tm_hour, utc.tm_min, utc.tm_sec, (unsigned)utc.tm_wday, in_seconds);
    } else {
        ESP_LOGW(TAG, "weekly arm failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t muse_rtc_disarm(void)
{
    s_wake_armed = false;
    if (!s_present) {
        return ESP_OK;
    }
    /* AIE off and AF cleared: with AF left set the chip holds INT low and the
     * next power_off() would wake on it the instant it slept. */
    uint8_t c2 = 0;
    if (rd(PCF85063A_RTC_CTRL_2_ADDR, &c2, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    const uint8_t nc2 = c2 & (uint8_t)~(PCF85063A_RTC_CTRL_2_AIE | PCF85063A_RTC_CTRL_2_AF);
    if (nc2 == c2) {
        return ESP_OK;
    }
    return wr(PCF85063A_RTC_CTRL_2_ADDR, &nc2, 1);
}

bool muse_rtc_alarm_pending(void)
{
    if (!s_present) {
        return false;
    }
    uint8_t c2 = 0;
    if (rd(PCF85063A_RTC_CTRL_2_ADDR, &c2, 1) != ESP_OK) {
        return false;
    }
    return (c2 & PCF85063A_RTC_CTRL_2_AF) != 0;
}

bool muse_rtc_wake_armed(void)
{
    return s_wake_armed;
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

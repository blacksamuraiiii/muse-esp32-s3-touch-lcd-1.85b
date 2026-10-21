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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The clock: NXP PCF85063(A) at I2C 0x51, driven through Waveshare's own
 * espressif component (waveshare/pcf85063a), which owns the device handle and
 * the transport. What this file adds is the part a device needs and the
 * component does not do:
 *
 *  - The chip keeps UTC. The system clock keeps UTC too, so the two can be
 *    copied one into the other; local time exists only where it is shown, and
 *    comes from localtime_r() with CONFIG_MUSE_TIMEZONE.
 *  - Validity. The chip sets Seconds.OS (bit 7) when its oscillator stopped -
 *    first power-on, or a dead backup cell - and the counters mean nothing
 *    until a write clears it. The component masks that bit off; this does not.
 *  - Year. The chip stores two BCD digits and has no century; they are 2000+yy
 *    here. (The component adds YEAR_OFFSET = 1970 instead, which round-trips
 *    through itself but leaves 2056 in the chip.)
 *  - A write is bracketed by STOP: the datasheet wants the counters held while
 *    04h-0Ah is replaced, or a carry can tear the value.
 *
 * The alarm registers are deliberately left unarmed: nothing in the UI schedules
 * a wake yet. What is wired is the line and the mask - RTC_INT (GPIO6) folds
 * into the deep-sleep wake mask in the board's power_off() as soon as
 * muse_rtc_wake_armed() says an alarm is pending - so the version that starts
 * scheduling wakes only has to fill in the register side below.
 */

#define MUSE_RTC_ADDR 0x51
/* RTC_INT on the Waveshare ESP32-S3-Touch-LCD-1.85B (open drain, active low,
 * needs a pull-up in the RTC domain). It joins BOOT on the deep-sleep wake
 * mask; the alarm that would raise it is not implemented yet. */
#define MUSE_RTC_INT_GPIO 6

/* Probe 0x51, start the clock in 24-hour mode, clear any latched alarm state.
 * Returns ESP_ERR_NOT_FOUND when no chip answers - the board then runs on the
 * system clock alone, and muse_rtc_clock_str() reports no time. */
esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus);
bool muse_rtc_present(void);
/* False until a read has produced a sane date (OS clear, BCD in range). */
bool muse_rtc_time_valid(void);

/* Chip -> *utc. False when absent, the oscillator stopped, or the BCD is
 * nonsense. */
bool muse_rtc_get(struct tm *utc);
/* *utc -> chip, with the counters stopped for the write. */
esp_err_t muse_rtc_set(const struct tm *utc);

/* Boot: if the system clock looks unset (< 2025-01-01) and the chip holds a
 * valid time, settimeofday() from it. True when it did. */
bool muse_rtc_adopt(void);
/* System clock -> chip (UTC). Refuses while the system clock looks unset. */
esp_err_t muse_rtc_push(void);

/* Sets both clocks from UTC fields, for a bench with no network. The system
 * clock takes it as well, so the two agree immediately. */
esp_err_t muse_rtc_set_utc_fields(int year, int month, int day,
                                  int hour, int minute, int second);

/* Sets CONFIG_MUSE_TIMEZONE, adopts the chip's time, and starts the task that
 * brings SNTP up once Wi-Fi associates and writes the corrected time back.
 * Call once, after the board's init() and after Wi-Fi is initialised. */
void muse_time_start(void);

/* "HH:MM" in local time, for the status line. False when there is no time
 * worth showing yet (no chip and no network). */
bool muse_rtc_clock_str(char *buf, size_t len);
/* "YYYY-MM-DD HH:MM:SS <zone>" in local time, for the console. */
bool muse_rtc_stamp_str(char *buf, size_t len);

/*
 * The alarm, reserved for a scheduled wake - the register side is not written
 * yet, so these are a stable seam rather than a feature:
 *
 *  - muse_rtc_arm_wakeup_in() validates its argument, logs the refusal and
 *    returns ESP_ERR_NOT_SUPPORTED. Nothing is armed.
 *  - muse_rtc_wake_armed() is what the board's power_off() asks before folding
 *    MUSE_RTC_INT_GPIO into the ext1 deep-sleep mask. It is false until an
 *    alarm is really armed, so deep-sleep behaviour is unchanged by the mask
 *    being ext1 now.
 *  - After a real alarm the waker must call muse_rtc_disarm(): AF stays set
 *    until cleared, and a set AF with AIE=1 holds INT low - the chip would then
 *    re-wake the instant it slept.
 */
esp_err_t muse_rtc_arm_wakeup_in(uint32_t in_seconds);
esp_err_t muse_rtc_disarm(void);
bool muse_rtc_alarm_pending(void);
bool muse_rtc_wake_armed(void);

#ifdef __cplusplus
}
#endif

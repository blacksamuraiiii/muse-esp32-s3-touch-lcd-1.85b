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
 * QMI8658A gestures: wake on motion, mute on face-down.
 *
 * Register facts, QST QMI8658A datasheet Rev A. The numbers below were read
 * off the same part on this board, so they are the ones that matter here:
 *
 *   CTRL1   0x02  bit6 ADDR_AUTO_INC, bit3 INT1_EN
 *   CTRL2   0x03  [7:4] accelerometer full scale, [3:0] ODR
 *   CTRL7   0x08  bit0 aEN (accelerometer on), bit5 DRDY_DIS
 *   CTRL8   0x09  bit7 handshake status goes to STATUSINT, not INT1
 *   CTRL9   0x0A  command register; 0x08 = write the wake-on-motion setting
 *   CAL1_L  0x0B  wake-on-motion threshold, 1 mg per LSB
 *   CAL1_H  0x0C  [7:6] output pin and its idle level, [5:0] blanking time
 *   STATUSINT 0x2D bit7 command done
 *   STATUS1 0x2F  bit5 any-motion, bit2 wake-on-motion (sticky until read)
 *   AX_L    0x35  six bytes: X, Y, Z little endian
 *
 * Two things killed the tap engine this replaces, and both are handled here:
 * a truncated I2C frame (automatic light sleep stopping the APB clock mid
 * transfer) and a latched STATUS1 nobody read. Every transaction runs under a
 * no-light-sleep PM lock, and the caller reads STATUS1 whether the screen is on
 * or off.
 */

#include "muse_imu.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "muse.imu";

/* engine_start() prints the resting vector, and both helpers live below it with
 * the rest of the register access. Declared here so the build does not depend on
 * an implicit declaration (an error from GCC 14 on). */
static bool accel_mg(int out[3]);
static int vec_mag(const int v[3]);

#define REG_WHO_AM_I    0x00
#define REG_CTRL1       0x02
#define REG_CTRL2       0x03
#define REG_CTRL7       0x08
#define REG_CTRL8       0x09
#define REG_CTRL9       0x0A
#define REG_CAL1_L      0x0B
#define REG_CAL1_H      0x0C
#define REG_STATUSINT   0x2D
#define REG_STATUS1     0x2F
#define REG_ACC_X_L     0x35
#define REG_RESET       0x60

#define RESET_CMD       0xB0
#define WHO_AM_I_QMI8658 0x05

#define CTRL1_AUTO_INC  (1 << 6)
#define CTRL1_INT1_EN   (1 << 3)
#define CTRL7_AEN       (1 << 0)
#define CTRL7_DRDY_DIS  (1 << 5)
#define CTRL8_HANDSHAKE_STATUSINT (1 << 7)

/* 2 g full scale, low-power 128 Hz: the wake-on-motion engine runs off its own
 * low-power sampler, and this is the rate the vendor's reference driver pairs
 * with it. It is also what the face-down read uses, which is why the data
 * registers are still live in this mode. */
#define CTRL2_WOM_2G_LP128HZ 0x0Cu

#define CTRL_CMD_ACK            0x00
#define CTRL_CMD_WRITE_WOM      0x08

/* Wake-on-motion output: pin selector 00 = INT1, idle level 0 (active high,
 * which is what GPIO8 is armed on), blanking 32 samples - about 250 ms at the
 * 128 Hz low-power rate, so one movement raises the line once. */
#define WOM_PIN_INT1_ACTIVE_HIGH 0x00u
#define WOM_BLANKING_SAMPLES     0x20u

#define STATUS1_ANY_MOTION (1 << 5)
#define STATUS1_WOM        (1 << 2)

#define ACC_LSB_PER_G 16384   /* 2 g full scale */

/* Face-down: gravity pointing this far *against* the direction it pointed when
 * the screen was last known to be up. Three reads running, is a deliberate turn
 * rather than a wobble. */
#define FACE_DOWN_DOT_MG  700
#define FACE_DOWN_HITS    3
#define FACE_DOWN_PERIOD_US (200 * 1000)

/* Calibration: gravity alone is 1000 mg, so anything well inside that band is a
 * device at rest and anything outside it is a hand still moving. */
#define CAL_MIN_MG  700
#define CAL_MAX_MG  1300
#define CAL_SAMPLES 4

/* A reading is only allowed to decide anything while the device is near rest;
 * mid-flip the vector swings through every direction on the way. */
#define FACE_STILL_MIN_MG 600
#define FACE_STILL_MAX_MG 1400

#define RECOVER_MIN_MS 10000

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm;
#endif

static bool s_ready;
static uint8_t s_exp_ctrl1, s_exp_ctrl2, s_exp_ctrl7, s_exp_ctrl8;
static uint8_t s_exp_cal[2];
static uint32_t s_recoveries, s_rearms;
static TickType_t s_last_recover;

static int s_face_hits;
static int64_t s_face_next_us;
static bool s_face_down;
/* Which way gravity points when the screen is up, in mg. Learned at start-up
 * instead of assumed: the axis and the sign both depend on how the part was
 * rotated when it was placed on this PCB, and a wrong guess here is invisible
 * (the gesture simply never fires) rather than loud. */
static int s_up_mg[3];
static bool s_up_known;
static int s_cal_n;
static int32_t s_cal_sum[3];

/* ------------------------------------------------------------------ bus */

static void bus_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void bus_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* Every transaction takes the PM lock: with automatic light sleep armed, the
 * APB clock can otherwise stop between two bytes, and the IMU reads the
 * truncated frame as a register write. */
static esp_err_t bus_write(const uint8_t *buf, size_t len)
{
#if CONFIG_PM_ENABLE
    if (s_pm) esp_pm_lock_acquire(s_pm);
#endif
    esp_err_t err = i2c_master_transmit(s_dev, buf, len, 100);
#if CONFIG_PM_ENABLE
    if (s_pm) esp_pm_lock_release(s_pm);
#endif
    return err;
}

static esp_err_t bus_read(uint8_t reg, uint8_t *out, size_t len)
{
#if CONFIG_PM_ENABLE
    if (s_pm) esp_pm_lock_acquire(s_pm);
#endif
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, out, len, 100);
#if CONFIG_PM_ENABLE
    if (s_pm) esp_pm_lock_release(s_pm);
#endif
    return err;
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    bus_lock();
    esp_err_t err = bus_write(buf, sizeof(buf));
    bus_unlock();
    return err;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *out)
{
    bus_lock();
    esp_err_t err = bus_read(reg, out, 1);
    bus_unlock();
    return err;
}

/* CTRL9 protocol: the command runs, STATUSINT.bit7 goes to 1, and the host has
 * to acknowledge before the next one. */
static esp_err_t run_command(uint8_t cmd)
{
    esp_err_t err = write_reg(REG_CTRL9, cmd);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 20; i++) {
        uint8_t st = 0;
        if ((err = read_reg(REG_STATUSINT, &st)) != ESP_OK) {
            return err;
        }
        if (st & 0x80) {
            return write_reg(REG_CTRL9, CTRL_CMD_ACK);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "CTRL9 command 0x%02x not done", cmd);
    return ESP_FAIL;
}

/* ------------------------------------------------------------- the engine */

/* The body of init, also used by rearm(). A failed attempt leaves s_ready as it
 * was: one glitch on a congested bus must not disable the wake path until the
 * next reboot. */
static esp_err_t engine_start(bool quiet)
{
    const bool was_ready = s_ready;
    esp_err_t err;
    s_ready = false;

    uint8_t who = 0;
    if ((err = read_reg(REG_WHO_AM_I, &who)) != ESP_OK) {
        ESP_LOGW(TAG, "no IMU on the bus: %s", esp_err_to_name(err));
        goto out;
    }
    if (who != WHO_AM_I_QMI8658) {
        ESP_LOGW(TAG, "IMU WHO_AM_I 0x%02x, expected 0x%02x", who, WHO_AM_I_QMI8658);
        err = ESP_ERR_NOT_FOUND;
        goto out;
    }

    /* Reset first: power-on register state is not guaranteed, and a reset
     * clears whatever a half-written frame left behind. 15 ms is the
     * datasheet's figure, 20 ms leaves margin. */
    if ((err = write_reg(REG_RESET, RESET_CMD)) != ESP_OK) {
        ESP_LOGW(TAG, "IMU reset failed: %s", esp_err_to_name(err));
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Auto-increment for the burst writes below, INT1 push-pull enabled, and
     * the CTRL9 handshake kept on STATUSINT so configuring never pulls INT1. */
    if ((err = write_reg(REG_CTRL1, CTRL1_AUTO_INC | CTRL1_INT1_EN)) != ESP_OK ||
        (err = write_reg(REG_CTRL8, CTRL8_HANDSHAKE_STATUSINT)) != ESP_OK) {
        goto out;
    }
    /* Parameters go in with the sensors stopped (datasheet 10.3). */
    if ((err = write_reg(REG_CTRL7, 0x00)) != ESP_OK) {
        goto out;
    }
    /* Range and ODR before the engine is told to use them. */
    if ((err = write_reg(REG_CTRL2, CTRL2_WOM_2G_LP128HZ)) != ESP_OK) {
        goto out;
    }

    /* ---- TEMP PROBE A: does the plain accelerometer path produce data at
     * all, before the wake-on-motion command touches the chip? ---- */
    {
        int g[3] = { 0, 0, 0 };
        uint8_t st0 = 0, c7 = 0;
        write_reg(REG_CTRL7, CTRL7_AEN);
        vTaskDelay(pdMS_TO_TICKS(150));
        bool ok = accel_mg(g);
        read_reg(0x2E, &st0);
        read_reg(REG_CTRL7, &c7);
        ESP_LOGW(TAG, "PROBE-A accel-on: ok=%d %d/%d/%d mg STATUS0=0x%02x CTRL7=0x%02x",
                 (int)ok, g[0], g[1], g[2], st0, c7);
        write_reg(REG_CTRL7, 0x00);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* The wake-on-motion setting itself: threshold, then pin and blanking,
     * then the command that makes the chip adopt both. Writing the two
     * registers without the command leaves the engine on its power-on
     * defaults, which is the trap the vendor's helper falls into. */
    const uint8_t thr = (uint8_t)(CONFIG_MUSE_IMU_WAKE_MG > 255 ? 255 : CONFIG_MUSE_IMU_WAKE_MG);
    const uint8_t cfg = (uint8_t)((WOM_PIN_INT1_ACTIVE_HIGH << 6) | WOM_BLANKING_SAMPLES);
    if ((err = write_reg(REG_CAL1_L, thr)) != ESP_OK ||
        (err = write_reg(REG_CAL1_H, cfg)) != ESP_OK) {
        ESP_LOGW(TAG, "wake-on-motion parameters rejected: %s", esp_err_to_name(err));
        goto out;
    }
    if ((err = run_command(CTRL_CMD_WRITE_WOM)) != ESP_OK) {
        ESP_LOGW(TAG, "wake-on-motion setting not applied: %s", esp_err_to_name(err));
        goto out;
    }

    /* Accelerometer on, data-ready kept off INT1 (the line belongs to the
     * motion engine), and the data registers live for the face-down read. */
    if ((err = write_reg(REG_CTRL7, CTRL7_AEN | CTRL7_DRDY_DIS)) != ESP_OK) {
        goto out;
    }

    /* ---- TEMP PROBE B: the same read after the wake-on-motion command. If A
     * had a vector and B has zeros, the engine owns the data path. ---- */
    {
        int g[3] = { 0, 0, 0 };
        uint8_t st0 = 0, c7 = 0;
        vTaskDelay(pdMS_TO_TICKS(150));
        bool ok = accel_mg(g);
        read_reg(0x2E, &st0);
        read_reg(REG_CTRL7, &c7);
        ESP_LOGW(TAG, "PROBE-B after-wom: ok=%d %d/%d/%d mg STATUS0=0x%02x CTRL7=0x%02x",
                 (int)ok, g[0], g[1], g[2], st0, c7);
    }

    s_exp_ctrl1 = CTRL1_AUTO_INC | CTRL1_INT1_EN;
    s_exp_ctrl2 = CTRL2_WOM_2G_LP128HZ;
    s_exp_ctrl7 = CTRL7_AEN | CTRL7_DRDY_DIS;
    s_exp_ctrl8 = CTRL8_HANDSHAKE_STATUSINT;
    s_exp_cal[0] = thr;
    s_exp_cal[1] = cfg;
    s_ready = true;
    /* Which way up the panel is gets learned from the first readings the caller
     * takes (muse_imu_face_down); this line is the same vector, printed once so
     * a boot log says whether the accelerometer is producing data at all. */
    {
        int g[3];
        if (accel_mg(g)) {
            ESP_LOGI(TAG, "accel %d/%d/%d mg (%d mg) - this becomes the screen-up reference",
                     g[0], g[1], g[2], vec_mag(g));
        } else {
            ESP_LOGW(TAG, "accelerometer not readable: face-down will never fire");
        }
    }
    if (quiet) {
        ESP_LOGD(TAG, "wake-on-motion restarted (#%u)", s_rearms);
    } else {
        ESP_LOGI(TAG, "wake-on-motion on: %d mg, INT1 -> GPIO%d (CTRL2=0x%02x CTRL7=0x%02x)",
                 CONFIG_MUSE_IMU_WAKE_MG, MUSE_IMU_INT_GPIO, s_exp_ctrl2, s_exp_ctrl7);
    }
    return ESP_OK;

out:
    s_ready = was_ready;
    return err;
}

esp_err_t muse_imu_init(i2c_master_dev_handle_t dev)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
#if CONFIG_PM_ENABLE
    if (!s_pm) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "muse_imu", &s_pm) != ESP_OK) {
            ESP_LOGW(TAG, "no PM lock: a light sleep mid-transfer can still corrupt the IMU");
            s_pm = NULL;
        }
    }
#endif
    s_dev = dev;
    s_face_hits = 0;
    s_face_next_us = 0;
    s_face_down = false;
    /* A fresh boot re-learns the reference: the device is in someone's hand at a
     * different angle than last time, and the old vector may be a session old. */
    muse_imu_face_calibrate();
    return engine_start(false);
}

bool muse_imu_ready(void)
{
    return s_ready;
}

bool muse_imu_moved(void)
{
    if (!s_dev) {
        return false;
    }
    /* Read even when the engine is not known to be configured: the read is what
     * drops INT1, and a chip still running after a half-failed init would
     * otherwise hold the line high for good. */
    uint8_t st = 0;
    if (read_reg(REG_STATUS1, &st) != ESP_OK) {
        return false;
    }
    /* Reading STATUS1 clears the sticky bits and releases the line. Both bits
     * are accepted: the motion engine reports wake-on-motion, but a chip
     * configured through the any-motion path reports the other. */
    return (st & (STATUS1_ANY_MOTION | STATUS1_WOM)) != 0;
}

/* One accelerometer read, in milli-g. Shared by the face-down decision, the
 * start-up calibration and the console dump. */
static bool accel_mg(int out[3])
{
    if (!s_dev) {
        return false;
    }
    uint8_t raw[6] = { 0 };
    bus_lock();
    esp_err_t err = bus_read(REG_ACC_X_L, raw, sizeof(raw));
    bus_unlock();
    if (err != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out[i] = (int16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8)) * 1000 / ACC_LSB_PER_G;
    }
    return true;
}

static int vec_mag(const int v[3])
{
    const float sq = (float)v[0] * v[0] + (float)v[1] * v[1] + (float)v[2] * v[2];
    return (int)sqrtf(sq);
}

int muse_imu_bump_mg(void)
{
    int g[3];
    if (!accel_mg(g)) {
        return -1;
    }
    /* Resting, the magnitude is 1 g; anything above it is motion. int64 because
     * three squared axes overflow int32 at this full scale. */
    int64_t sq = (int64_t)g[0] * g[0] + (int64_t)g[1] * g[1] + (int64_t)g[2] * g[2];
    int64_t rest = 1000 * 1000;
    if (sq <= rest) {
        return 0;
    }
    return (int)((sq - rest) / (2 * 1000));
}

bool muse_imu_face_down(void)
{
    if (!s_dev) {
        return false;
    }
    /* Throttled: the caller runs at the audio chunk rate, and one bus read per
     * 200 ms is already faster than anyone can turn the thing over. */
    int64_t now = esp_timer_get_time();
    if (now < s_face_next_us) {
        return s_face_down;
    }
    s_face_next_us = now + FACE_DOWN_PERIOD_US;

    int g[3];
    if (!accel_mg(g)) {
        s_face_hits = 0;
        return s_face_down;
    }
    const int mag = vec_mag(g);

#if CONFIG_MUSE_IMU_FACE_DOWN_POSITIVE
    /* Manual override for a board whose part is mounted the obvious way: the
     * screen faces down when gravity reads +Z. */
    if (!s_up_known) {
        s_up_mg[0] = 0;
        s_up_mg[1] = 0;
        s_up_mg[2] = -1000;
        s_up_known = true;
    }
#endif

    if (!s_up_known) {
        /* Learning the reference. At rest gravity is 1000 mg whatever the
         * mounting, so a few consecutive readings inside that band describe the
         * direction the screen was facing when the device came up - which is
         * face up, because that is how anyone reads it. */
        if (mag < CAL_MIN_MG || mag > CAL_MAX_MG) {
            s_cal_n = 0;
            s_cal_sum[0] = s_cal_sum[1] = s_cal_sum[2] = 0;
            return false;
        }
        s_cal_sum[0] += g[0];
        s_cal_sum[1] += g[1];
        s_cal_sum[2] += g[2];
        if (++s_cal_n < CAL_SAMPLES) {
            return false;
        }
        s_up_mg[0] = s_cal_sum[0] / s_cal_n;
        s_up_mg[1] = s_cal_sum[1] / s_cal_n;
        s_up_mg[2] = s_cal_sum[2] / s_cal_n;
        s_up_known = true;
        ESP_LOGI(TAG, "screen-up gravity %d/%d/%d mg (%d mg): face down is the opposite",
                 s_up_mg[0], s_up_mg[1], s_up_mg[2], vec_mag(s_up_mg));
        return false;
    }

    if (mag < FACE_STILL_MIN_MG || mag > FACE_STILL_MAX_MG) {
        /* Still in someone's hand, mid-turn: the vector swings through every
         * direction on the way round, so none of it counts as a decision. */
        s_face_hits = 0;
        return s_face_down;
    }

    /* How much of gravity points the way it did when the screen was up. +1000
     * is flat on its back, -1000 is flat on its face, 0 is on edge. */
    const int32_t dot = ((int32_t)g[0] * s_up_mg[0] + (int32_t)g[1] * s_up_mg[1] +
                         (int32_t)g[2] * s_up_mg[2]) / 1000;
    if (dot <= -FACE_DOWN_DOT_MG) {
        if (s_face_hits < FACE_DOWN_HITS) {
            s_face_hits++;
        }
        if (s_face_hits >= FACE_DOWN_HITS && !s_face_down) {
            s_face_down = true;
            ESP_LOGI(TAG, "face down (gravity %d/%d/%d mg, %d mg against up): muting",
                     g[0], g[1], g[2], (int)dot);
        }
    } else {
        s_face_hits = 0;
        if (s_face_down) {
            s_face_down = false;
            ESP_LOGI(TAG, "face up (gravity %d/%d/%d mg, %d mg with up)",
                     g[0], g[1], g[2], (int)dot);
        }
    }
    return s_face_down;
}

/* ------------------------------------------------------- wake posture */

/* Gravity when the screen went dark, in mg. Not the same thing as s_up_mg:
 * that one is "which way was up when the device booted", this one is "which way
 * was it pointing when the user put it down", and a lift has to change away
 * from the second. */
static int s_sleep_mg[3];
static bool s_sleep_known;

/* A reading may only decide anything near rest: mid-swing the vector points
 * everywhere on the way round, and so does the angle against the reference. */
#define WAKE_STILL_MIN_MG 750
#define WAKE_STILL_MAX_MG 1250

static int angle_deg_between(const int a[3], const int b[3])
{
    const float dot = (float)a[0] * b[0] + (float)a[1] * b[1] + (float)a[2] * b[2];
    const float ma = sqrtf((float)a[0] * a[0] + (float)a[1] * a[1] + (float)a[2] * a[2]);
    const float mb = sqrtf((float)b[0] * b[0] + (float)b[1] * b[1] + (float)b[2] * b[2]);
    if (ma < 1.0f || mb < 1.0f) {
        return -1;
    }
    float c = dot / (ma * mb);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return (int)(acosf(c) * 57.29578f);
}

void muse_imu_note_sleep_posture(void)
{
    int g[3];
    if (!accel_mg(g)) {
        return;   /* bus error: the old reference is still the last good one */
    }
    const int mag = vec_mag(g);
    if (mag < WAKE_STILL_MIN_MG || mag > WAKE_STILL_MAX_MG) {
        /* Put down while still being carried: this vector is a snapshot of a
         * swing, not a posture. Keep what we had. */
        ESP_LOGD(TAG, "sleep posture not updated: %d mg is not at rest", mag);
        return;
    }
    s_sleep_mg[0] = g[0];
    s_sleep_mg[1] = g[1];
    s_sleep_mg[2] = g[2];
    s_sleep_known = true;
    ESP_LOGI(TAG, "sleep posture %d/%d/%d mg (%d mg)", g[0], g[1], g[2], mag);
}

int muse_imu_posture_delta_deg(void)
{
    int g[3];
    if (!accel_mg(g)) {
        return -1;
    }
    if (s_sleep_known) {
        return angle_deg_between(g, s_sleep_mg);
    }
    if (s_up_known) {
        return angle_deg_between(g, s_up_mg);
    }
    return -1;
}

bool muse_imu_wake_is_deliberate(int min_deg)
{
    int g[3];
    if (!accel_mg(g)) {
        return true;    /* unreadable: let the user see the screen */
    }
    const int mag = vec_mag(g);
    if (mag < WAKE_STILL_MIN_MG || mag > WAKE_STILL_MAX_MG) {
        return false;   /* still in motion: wait for the next event, do not wake */
    }
    int deg = -1;
    if (s_sleep_known) {
        deg = angle_deg_between(g, s_sleep_mg);
    } else if (s_up_known) {
        deg = angle_deg_between(g, s_up_mg);
    }
    if (deg < 0) {
        return true;    /* no reference at all: never lock the screen out */
    }
    ESP_LOGD(TAG, "posture %d deg (%d mg), need %d", deg, mag, min_deg);
    return deg >= min_deg;
}

void muse_imu_face_calibrate(void)
{
    s_up_known = false;
    s_cal_n = 0;
    s_cal_sum[0] = s_cal_sum[1] = s_cal_sum[2] = 0;
    s_face_hits = 0;
    s_face_down = false;
    ESP_LOGI(TAG, "face-down reference cleared: hold the screen up, it re-learns in ~1 s");
}

void muse_imu_log_face(const char *why)
{
    int g[3] = { 0, 0, 0 };
    const bool ok = accel_mg(g);
    int dot = 0;
    if (ok && s_up_known) {
        dot = (int)(((int32_t)g[0] * s_up_mg[0] + (int32_t)g[1] * s_up_mg[1] +
                     (int32_t)g[2] * s_up_mg[2]) / 1000);
    }
    ESP_LOGI(TAG,
             "%s: accel %d/%d/%d mg (|g| %d), dot %d, up %d/%d/%d known=%d, "
             "hits %d/%d, down=%d, threshold %d",
             why, g[0], g[1], g[2], ok ? vec_mag(g) : -1, dot,
             s_up_mg[0], s_up_mg[1], s_up_mg[2], (int)s_up_known,
             s_face_hits, FACE_DOWN_HITS, (int)s_face_down, FACE_DOWN_DOT_MG);
}

/* ------------------------------------------------- health and recovery */

void muse_imu_log_diag(const char *why)
{
    uint8_t c1 = 0, c2 = 0, c7 = 0, c8 = 0, si = 0, s1 = 0, cal[2] = { 0 };
    if (!s_dev) {
        ESP_LOGW(TAG, "%s: no IMU device handle", why);
        return;
    }
    if (read_reg(REG_CTRL1, &c1) != ESP_OK || read_reg(REG_CTRL2, &c2) != ESP_OK ||
        read_reg(REG_CTRL7, &c7) != ESP_OK || read_reg(REG_CTRL8, &c8) != ESP_OK ||
        read_reg(REG_STATUSINT, &si) != ESP_OK || read_reg(REG_STATUS1, &s1) != ESP_OK) {
        ESP_LOGW(TAG, "%s: IMU not readable", why);
        return;
    }
    bus_lock();
    (void)bus_read(REG_CAL1_L, cal, sizeof(cal));
    bus_unlock();
    ESP_LOGW(TAG,
             "%s: ready=%d int1=%d CTRL1=0x%02x CTRL2=0x%02x(want 0x%02x) "
             "CTRL7=0x%02x(want 0x%02x) CTRL8=0x%02x(want 0x%02x) STATUSINT=0x%02x "
             "STATUS1=0x%02x wom=%u/%u recoveries=%u rearms=%u",
             why, (int)s_ready, gpio_get_level(MUSE_IMU_INT_GPIO), c1, c2, s_exp_ctrl2,
             c7, s_exp_ctrl7, c8, s_exp_ctrl8, si, s1, cal[0], cal[1],
             s_recoveries, s_rearms);
}

/* Re-runs init() when the chip no longer holds what init() wrote. Rate-limited:
 * a chip that keeps drifting faster than this needs a hardware look, not a
 * tighter loop. */
static bool recover(const char *why)
{
    TickType_t now = xTaskGetTickCount();
    if (s_last_recover && now - s_last_recover < pdMS_TO_TICKS(RECOVER_MIN_MS)) {
        ESP_LOGW(TAG, "IMU config still wrong (%s), waiting before another re-init", why);
        return false;
    }
    s_last_recover = now;
    muse_imu_log_diag(why);
    s_recoveries++;
    if (muse_imu_init(s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "IMU re-init failed (#%u)", s_recoveries);
        return false;
    }
    ESP_LOGW(TAG, "IMU re-initialised (#%u)", s_recoveries);
    return true;
}

bool muse_imu_check(bool full)
{
    if (!s_dev) {
        return false;
    }
    if (!s_ready) {
        /* Not "nothing to check": a boot that failed to configure the engine
         * leaves this false for the rest of the session, and that is exactly
         * the state worth repairing. */
        return recover("engine not configured");
    }
    uint8_t c1 = 0, c2 = 0, c7 = 0, c8 = 0;
    if (read_reg(REG_CTRL1, &c1) != ESP_OK || read_reg(REG_CTRL2, &c2) != ESP_OK ||
        read_reg(REG_CTRL7, &c7) != ESP_OK || read_reg(REG_CTRL8, &c8) != ESP_OK) {
        return false;   /* bus trouble: leave it to the next poll */
    }
    if (c1 != s_exp_ctrl1 || c2 != s_exp_ctrl2 || c7 != s_exp_ctrl7 || c8 != s_exp_ctrl8) {
        return recover("config drift");
    }
    if (full) {
        uint8_t cal[2] = { 0 };
        bus_lock();
        esp_err_t err = bus_read(REG_CAL1_L, cal, sizeof(cal));
        bus_unlock();
        if (err == ESP_OK && memcmp(cal, s_exp_cal, sizeof(cal)) != 0) {
            return recover("wake-on-motion parameters lost");
        }
    }
    return true;
}

void muse_imu_rearm(void)
{
    if (!s_dev) {
        return;   /* no IMU was ever found: nothing to restart */
    }
    /* A full restart, not a bare enable toggle: the parameters have to go back
     * in with the sensors stopped, and a toggle would leave a silently dead
     * engine behind if the chip had dropped them. This is the only repair
     * available for an engine that wedged with its configuration intact. */
    s_rearms++;
    s_last_recover = xTaskGetTickCount();
    if (engine_start(true) != ESP_OK) {
        ESP_LOGW(TAG, "wake-on-motion restart failed (#%u)", s_rearms);
    }
}

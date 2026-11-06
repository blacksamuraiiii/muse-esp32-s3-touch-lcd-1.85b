/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_imu.c -- the board's QMI8658 6-axis IMU, see muse_imu.h.
 *
 * Driver: waveshare/qmi8658 v2.0.2 from the ESP Component Registry
 * (declared in main/idf_component.yml, linked through the muse
 * component's PRIV_REQUIRES). Its qmi8658_init() writes CTRL1 0x60,
 * sets accel 8G / 1000 Hz and gyro 512 dps / 1000 Hz, and enables
 * accel + gyro; this file then narrows it to what the game needs
 * (accel 2G for finer tilt resolution, 125 Hz ODR -- the game polls
 * at 20 Hz) and sleeps the sensors until a game wakes them.
 */
#include "muse_imu.h"

#include <stddef.h>

#include "esp_log.h"
#include "qmi8658.h"

static const char *TAG = "muse_imu";

static qmi8658_dev_t s_dev;
static bool s_ready = false;
static bool s_awake = false;

esp_err_t muse_imu_init(i2c_master_bus_handle_t bus)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 probe failed: %s (tilt unavailable, drag only)",
                 esp_err_to_name(err));
        return err;
    }
    /* Finer tilt resolution than the driver's 8G default (2G: 1 LSB is
     * ~0.06 mg, so a +/-3 degree dead zone is well above the noise),
     * and a slower output rate: the game polls at 20 Hz. */
    err = qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "accel range 2G failed: %s", esp_err_to_name(err));
        return err;
    }
    err = qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_125HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "accel ODR 125 Hz failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Sleep until a game wakes it: the init above left accel + gyro
     * enabled at 1000 Hz, which would draw current all day for a
     * sensor nothing else in this firmware uses. */
    err = qmi8658_enable_sensors(&s_dev, QMI8658_DISABLE_ALL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sensor sleep failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    s_awake = false;
    ESP_LOGI(TAG, "QMI8658 ready (accel 2G / 125 Hz, asleep)");
    return ESP_OK;
}

bool muse_imu_available(void)
{
    return s_ready;
}

void muse_imu_wake(void)
{
    if (!s_ready || s_awake) {
        return;
    }
    esp_err_t err = qmi8658_enable_sensors(&s_dev, QMI8658_ENABLE_ACCEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "accel wake failed: %s", esp_err_to_name(err));
        return;
    }
    s_awake = true;
}

void muse_imu_sleep(void)
{
    if (!s_ready || !s_awake) {
        return;
    }
    esp_err_t err = qmi8658_enable_sensors(&s_dev, QMI8658_DISABLE_ALL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sensor sleep failed: %s", esp_err_to_name(err));
        return;
    }
    s_awake = false;
}

esp_err_t muse_imu_read_accel(float *ax, float *ay, float *az)
{
    if (!s_ready || !s_awake) {
        return ESP_ERR_INVALID_STATE;
    }
    return qmi8658_read_accel_mps2(&s_dev, ax, ay, az);
}

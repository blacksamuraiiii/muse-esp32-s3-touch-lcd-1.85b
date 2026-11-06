/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_imu.h -- the board's QMI8658 6-axis IMU, one thin wrapper.
 *
 * Only the accelerometer is used (tilt: the gravity vector gives the
 * board's roll directly, no integration drift). The gyroscope is left
 * off: the UI smooths the tilt with a software low-pass, which is all
 * a 20 Hz game tick needs, and the gyro would only add current.
 *
 * Power discipline (1.4): the chip is probed and configured once at
 * board init (muse_imu_init, called by the board file that owns the
 * I2C bus), then put straight to sleep -- the sensors stay disabled
 * until a game wakes them (muse_imu_wake) and sleeps them again on
 * exit (muse_imu_sleep). Nothing in this firmware drove the QMI8658
 * before this file (1.4 removed the hand-written driver with the
 * gestures); the driver itself is the official waveshare/qmi8658
 * component from the ESP Component Registry (v2.0.2), not hand-written
 * registers (AGENTS.md: do not reinvent the wheel -- the hand-written
 * tap/WoM attempt cost a week and the interrupt engine was unreliable
 * on battery). Only polling is used: one read per game tick, no
 * interrupts, no wake-on-motion.
 *
 * Boards without the chip (or a failed probe) simply report
 * muse_imu_available() == false, and the game falls back to finger
 * drag alone -- the game never depends on the sensor.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the QMI8658 at 0x6B on `bus`, configure accel 2G / 125 Hz,
 * then disable the sensors until muse_imu_wake(). Idempotent; returns
 * ESP_ERR_NOT_FOUND when the chip does not answer (the board then
 * runs without tilt). Call once, from the board init that owns the
 * I2C bus. */
esp_err_t muse_imu_init(i2c_master_bus_handle_t bus);

/* True when the probe above succeeded. */
bool muse_imu_available(void);

/* Enable the accelerometer (game entry). No-op without the chip. */
void muse_imu_wake(void);

/* Disable all sensors (game exit). No-op without the chip. */
void muse_imu_sleep(void);

/* Latest acceleration in m/s^2. ESP_ERR_INVALID_STATE when the chip
 * is absent or asleep; any other error is an I2C failure. */
esp_err_t muse_imu_read_accel(float *ax, float *ay, float *az);

#ifdef __cplusplus
}
#endif

/*
 * Tap-to-wake on the board's QMI8658A. The board owns the I2C bus and the
 * interrupt line, so it passes the device handle for address 0x6B and wires the
 * IMU's INT1 GPIO as an extra light-sleep wake line
 * (muse_gpio_wake_line_add in muse_board.h).
 */

#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

/* The 1.85B wires QMI8658A INT1 to this pin through R51 (0R); INT2 is not
 * connected (schematic Rev 1.1, 6/9-Axis block). */
#define MUSE_IMU_TAP_GPIO GPIO_NUM_8

/* Configures the IMU's tap engine: accelerometer at 500 Hz, single/double tap
 * on INT1. Returns ESP_OK once a tap raises the line. */
esp_err_t muse_imu_tap_init(i2c_master_dev_handle_t dev);

/* True when init() got the engine running. */
bool muse_imu_tap_ready(void);

/* True once for a tap the IMU has latched: reads (and clears) STATUS1.bit1.
 * Safe from any task; cheap enough for the asleep poll. */
bool muse_imu_tap_tapped(void);

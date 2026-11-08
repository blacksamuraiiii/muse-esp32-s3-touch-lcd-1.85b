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

/* The desktop simulator has no QMI8658. The production code already handles
 * a board without it (muse_imu_available() == false makes the tilt games fall
 * back to their finger controls), so the simulator reports exactly that
 * instead of linking the real driver, which needs the ESP-IDF I2C master. */

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "muse_imu.h"

esp_err_t muse_imu_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    return ESP_ERR_NOT_FOUND;
}

bool muse_imu_available(void)
{
    return false;
}

void muse_imu_wake(void)
{
}

void muse_imu_sleep(void)
{
}

esp_err_t muse_imu_read_accel(float *ax, float *ay, float *az)
{
    (void)ax;
    (void)ay;
    (void)az;
    return ESP_ERR_INVALID_STATE;
}

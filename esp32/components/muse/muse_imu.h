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

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The QMI8658A behind the panel, used for two gestures and nothing else:
 *
 *  - Picking the gadget up wakes the screen. The chip's own wake-on-motion
 *    engine watches the accelerometer and pulls INT1 (GPIO8) when acceleration
 *    changes by more than CONFIG_MUSE_IMU_WAKE_MG. That line is registered as a
 *    light-sleep wake source by the board, so the CPU is asleep when it fires.
 *  - Turning the screen face down mutes what is playing, and a parked screen
 *    ignores the panel entirely. That is a plain accelerometer read compared
 *    against the gravity vector the device saw when it came up screen-up.
 *
 * The tap engine this file used to run is gone. It was configured for a single
 * or double knock, needed 500 Hz sampling and a FIFO reader to catch the
 * impulse on battery, and still missed taps often enough that the wake path was
 * not worth its complexity. Wake-on-motion needs no impulse to land inside a
 * poll window: the engine holds the decision and the line stays up until read.
 */

/* Waveshare ESP32-S3-Touch-LCD-1.85B: the IMU's INT1, active high. */
#define MUSE_IMU_INT_GPIO GPIO_NUM_8

/* Configures the accelerometer and its wake-on-motion engine. Idempotent: a
 * second call re-runs the whole configuration, which is also how a chip that
 * lost it is repaired. */
esp_err_t muse_imu_init(i2c_master_dev_handle_t dev);
bool muse_imu_ready(void);

/* True once for a motion event the chip has latched. Reading STATUS1 is what
 * drops INT1, so this must be called whether the screen is on or off - a latch
 * left set keeps the next sleep's wake line already satisfied. */
bool muse_imu_moved(void);

/* Screen towards the table, debounced over three reads and rate-limited to one
 * bus transaction per 200 ms. False when the IMU is absent or unreadable.
 *
 * "Towards the table" is measured against a gravity vector learned at start-up
 * rather than a fixed axis: the part's rotation on the PCB is not something the
 * firmware should have to know, and a wrong assumption here fails silently -
 * the gesture simply never fires. */
bool muse_imu_face_down(void);

/* Throws the learned reference away so the next second of readings re-learns
 * it. For a device that was booted face down, or when the console says the
 * reference is wrong. */
void muse_imu_face_calibrate(void);

/* One line with the live vector, the dot product against the reference and the
 * debounce state, for "the screen does not go dark when I turn it over". */
void muse_imu_log_face(const char *why);

/* Motion beyond gravity from one accelerometer read, in milli-g; -1 on a bus
 * error. For the log line that says whether the chip is still sampling. */
int muse_imu_bump_mg(void);

/* Compares the chip against what init() wrote and re-inits it when it drifted.
 * `full` also checks the wake-on-motion parameters (CAL1_L/H). Returns true
 * when the engine is configured. */
bool muse_imu_check(bool full);
/* Full restart, for an engine that wedged with its registers intact. */
void muse_imu_rearm(void);

/* Register dump at warning level, for "the screen did not wake". */
void muse_imu_log_diag(const char *why);

/* Remembers which way gravity pointed when the screen went dark. The wake check
 * below is measured against this, so "did the device change posture" needs no
 * assumption about how the part is rotated on the PCB. A reading taken while the
 * device is still moving is ignored and the previous reference kept: a snapshot
 * of a swing describes no posture at all. */
void muse_imu_note_sleep_posture(void);

/* True when gravity has turned by at least `min_deg` since that reference and
 * the device is at rest now - "it was lifted and turned towards a face", not
 * "it wobbled on the desk". True as well when no reference exists: a missing
 * baseline must light the screen, never lock the user out. */
bool muse_imu_wake_is_deliberate(int min_deg);

/* Degrees between the current gravity vector and the sleep reference; -1 when
 * either is unknown. For the log line and the console. */
int muse_imu_posture_delta_deg(void);

#ifdef __cplusplus
}
#endif

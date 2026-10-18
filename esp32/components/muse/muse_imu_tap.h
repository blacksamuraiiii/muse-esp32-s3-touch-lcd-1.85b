/*
 * Tap-to-wake on the board's QMI8658A. The board owns the I2C bus and the
 * interrupt line, so it passes the device handle for address 0x6B and wires the
 * IMU's INT1 GPIO as an extra light-sleep wake line
 * (muse_gpio_wake_line_add in muse_board.h).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

/* The 1.85B wires QMI8658A INT1 to this pin through R51 (0R); INT2 is not
 * connected (schematic Rev 1.1, 6/9-Axis block). */
#define MUSE_IMU_TAP_GPIO GPIO_NUM_8

/* Configures the IMU's tap engine: accelerometer at 500 Hz, single/double tap
 * on INT1. Returns ESP_OK once a tap raises the line. Safe to call again: that
 * is how the input task repairs a chip that lost its configuration. */
esp_err_t muse_imu_tap_init(i2c_master_dev_handle_t dev);

/* True when init() got the engine running. */
bool muse_imu_tap_ready(void);

/* True once for a tap the IMU has latched: reads (and clears) STATUS1.bit1.
 * Safe from any task; cheap enough for the asleep poll. Read it while the
 * screen is on too, or the latch keeps the engine from reporting the next tap. */
bool muse_imu_tap_tapped(void);

/* Motion above gravity in mG (milli-g) from one accelerometer read, -1 on a bus
 * error. The input task logs it beside STATUS1 on every knock, which is what
 * tells "the engine never saw this" from "it saw it and nobody read it". */
int muse_imu_tap_bump_mg(void);

/* Compares the chip against what init() wrote and re-runs init() when it
 * drifted. `full` also verifies the tap parameters (CAL1..CAL4), which costs
 * an extra burst read: do that once a minute, not on every poll. Returns true
 * when the engine is configured. */
bool muse_imu_tap_check(bool full);

/* Restarts the tap engine without touching its parameters. For an engine that
 * stopped detecting with its configuration intact, where check() sees nothing
 * wrong. Call it every ten seconds of sleep, not in the poll loop. */
void muse_imu_tap_rearm(void);

typedef struct {
    bool ready;
    uint8_t ctrl1, ctrl2, ctrl7, ctrl8, ctrl9;
    uint8_t statusint, status1, tap_status;
    uint8_t cal[8];
    bool ctrl7_ok, ctrl8_ok, cal_ok;
    unsigned recoveries, rearms;
} muse_imu_tap_diag_t;

/* One-shot register dump (this reads STATUS1, so it clears a latched tap). */
esp_err_t muse_imu_tap_diag(muse_imu_tap_diag_t *out);

/* The same dump as one WARN line, prefixed with `why`: the line to paste back
 * when a tap stops waking the screen. */
void muse_imu_tap_log_diag(const char *why);

/* One return from the input task's sleep: counts the wake, its cause
 * (GPIO/timer/other) and the IMU line's level at that moment. */
void muse_imu_tap_note_wake(int int1);

/* The RTC-resident record of what taps and repairs happened - the only
 * evidence that survives a battery session, where no cable is attached. */
void muse_imu_tap_diary_log(const char *why);

/* One accel-threshold wake: counted in the same RTC record, so the knock path
 * is visible on battery too. */
void muse_imu_tap_note_knock(int mg);

/* A wake that actually changed the state (set_asleep(false, ...) that stuck).
 * On battery this is the line between "nothing detected" and "detected but the
 * screen stayed dark". */
void muse_imu_tap_note_screen_wake(void);

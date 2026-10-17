#pragma once
/*
 * QMI8658 tap-to-wake for Waveshare ESP32-S3-Touch-LCD-1.85B.
 * See qmi8658_tap.c for wiring and register references.
 */
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pass the i2c_master_dev_handle_t for QMI8658 (addr 0x6B) on the board's
 * I2C bus. Must be called before qmi8658_tap_init(). */
void qmi8658_tap_set_i2c(void *i2c_dev_handle);

/* Configure tap engine + INT1 -> GPIO8 interrupt. */
esp_err_t qmi8658_tap_init(void);

/* Check-and-clear the chip's TAP flag (STATUS1.bit1). */
bool qmi8658_tap_poll(void);

/* True once per tap interrupt; clears both ISR flag and chip flag. */
bool qmi8658_tap_consumed(void);

/* Enter light sleep until a tap toggles GPIO8. Returns on wakeup. */
esp_err_t qmi8658_tap_sleep(void);

#ifdef __cplusplus
}
#endif

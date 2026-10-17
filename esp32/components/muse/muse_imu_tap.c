/*
 * Tap-to-wake on the QMI8658A IMU (the 1.85B's 6/9-axis part).
 *
 * The IMU's own tap engine watches the accelerometer; a tap raises INT1, which
 * the board wires to GPIO8 (see board_waveshare_s3_touch_lcd_185b.c). The
 * ESP32 light-sleeps on that line, so a tap on the case wakes the screen
 * without polling the IMU or keeping the CPU awake.
 *
 * Register names and the parameter tables follow the QMI8658A datasheet
 * (QST, Rev A): CTRL1 0x02, CTRL2 0x03, CTRL7 0x08, CTRL8 0x09, CTRL9 0x0A,
 * CAL1..CAL4 0x0B..0x12, STATUSINT 0x2D, STATUS1 0x2F, TAP_STATUS 0x59.
 * Note the shift against some community snippets: CTRL8 is 0x09, not 0x09's
 * neighbour, and only bits 0-7 of CAL4_H are the command-set selector.
 */

#include "muse_imu_tap.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse.imu";

#define REG_CTRL1       0x02    /* serial interface + INT1/INT2 enable */
#define REG_CTRL2       0x03    /* accelerometer ODR / full scale */
#define REG_CTRL7       0x08    /* sensor enable (aEN bit0, DRDY_DIS bit5) */
#define REG_CTRL8       0x09    /* motion engines + INT routing */
#define REG_CTRL9       0x0A    /* host command register */
#define REG_CAL1_L      0x0B    /* tap parameters, see Table 36 */
#define REG_STATUSINT   0x2D    /* bit7: CTRL9 command done, bit1: tap flag */
#define REG_STATUS1     0x2F    /* bit1: tap detected (sticky until read) */
#define REG_WHO_AM_I    0x00

#define CTRL1_AUTO_INC  (1 << 6)
#define CTRL1_INT1_EN   (1 << 3)
#define CTRL7_AEN       (1 << 0)
#define CTRL7_DRDY_DIS  (1 << 5)
#define CTRL8_HANDSHAKE_STATUSINT (1 << 7)   /* keep the CTRL9 handshake off INT1 */
#define CTRL8_TAP_TO_INT1         (1 << 6)
#define CTRL8_TAP_EN              (1 << 0)

#define CTRL_CMD_CONFIGURE_TAP  0x0C
#define CTRL_CMD_ACK            0x00

/* Tap parameters. The reference values in the datasheet (Table 35) are quoted
 * at 500 Hz, which is the ODR this sets; at 500 Hz one sample is 2 ms.
 * Adjust here when a tap on the case is missed or the case is too sensitive. */
#define TAP_PEAK_WINDOW_SAMPLES  20    /* 40 ms to reach the peak */
#define TAP_PRIORITY             0     /* only [2:0]; the tap axis, unused here */
#define TAP_WINDOW_SAMPLES       50    /* 100 ms of quiet before a second tap */
#define TAP_DTAP_WINDOW_SAMPLES  250   /* 500 ms to complete a double tap */
#define TAP_ALPHA                8     /* 0.0625: 7-bit fraction of 0.0078 */
#define TAP_GAMMA                32    /* 0.25 */
#define TAP_PEAK_MAG_THR         800   /* 0.8 g^2, units of 0.001 g^2 */
#define TAP_UDM_THR              400   /* 0.4 g, units of 0.001 g */
#define CTRL2_ODR_500HZ_2G       0x04  /* aODR = 4 (500 Hz accel-only), aFS = 0 */

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, 100);
}

/* CTRL9 protocol (datasheet 5.10.1): the command runs, STATUSINT.bit7 goes to
 * 1, and the host has to acknowledge with CTRL_CMD_ACK before the next one. */
static esp_err_t run_command(uint8_t cmd)
{
    esp_err_t err = write_reg(REG_CTRL9, cmd);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 20; i++) {
        uint8_t st = 0;
        err = read_reg(REG_STATUSINT, &st);
        if (err != ESP_OK) {
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

/* One of the two parameter sets (Table 36). Set 1 carries the windows and the
 * priority, set 2 the averaging ratios and the thresholds. */
static esp_err_t write_tap_params(const uint8_t p[8], uint8_t set)
{
    /* A burst write needs CTRL1.ADDR_AI (set in init), so CAL1_L..CAL4_H go in
     * one transfer. */
    uint8_t buf[9];
    buf[0] = REG_CAL1_L;
    memcpy(&buf[1], p, 8);
    buf[8] = (uint8_t)(set << 4);   /* CAL4_H[7:4] selects the set */
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        return err;
    }
    return run_command(CTRL_CMD_CONFIGURE_TAP);
}

esp_err_t muse_imu_tap_init(i2c_master_dev_handle_t dev)
{
    s_dev = dev;
    s_ready = false;
    uint8_t who = 0;
    esp_err_t err = read_reg(REG_WHO_AM_I, &who);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IMU on the bus: %s", esp_err_to_name(err));
        return err;
    }
    if (who != 0x05) {
        ESP_LOGW(TAG, "IMU WHO_AM_I 0x%02x, expected 0x05", who);
        return ESP_ERR_NOT_FOUND;
    }

    /* Auto-increment for burst writes, little-endian, INT1 push-pull enabled,
     * and the CTRL9 handshake kept on STATUSINT so configuring never pulls INT1. */
    err = write_reg(REG_CTRL1, CTRL1_AUTO_INC | CTRL1_INT1_EN);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_CTRL8, CTRL8_HANDSHAKE_STATUSINT);
    if (err != ESP_OK) {
        return err;
    }
    /* Parameters go in with the sensors stopped (datasheet 10.3). */
    err = write_reg(REG_CTRL7, 0x00);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t set1[8] = {
        TAP_PEAK_WINDOW_SAMPLES & 0xff, TAP_PRIORITY,
        TAP_WINDOW_SAMPLES & 0xff, (TAP_WINDOW_SAMPLES >> 8) & 0xff,
        TAP_DTAP_WINDOW_SAMPLES & 0xff, (TAP_DTAP_WINDOW_SAMPLES >> 8) & 0xff,
        0x00, 0x00,
    };
    const uint8_t set2[8] = {
        TAP_ALPHA, TAP_GAMMA,
        TAP_PEAK_MAG_THR & 0xff, (TAP_PEAK_MAG_THR >> 8) & 0xff,
        TAP_UDM_THR & 0xff, (TAP_UDM_THR >> 8) & 0xff,
        0x00, 0x00,
    };
    if ((err = write_tap_params(set1, 1)) != ESP_OK ||
        (err = write_tap_params(set2, 2)) != ESP_OK) {
        ESP_LOGW(TAG, "tap parameters rejected: %s", esp_err_to_name(err));
        return err;
    }

    /* Route the tap event to INT1, enable the engine, then start the
     * accelerometer at 500 Hz (the engine needs the samples). */
    err = write_reg(REG_CTRL8, CTRL8_HANDSHAKE_STATUSINT | CTRL8_TAP_TO_INT1 | CTRL8_TAP_EN);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(REG_CTRL2, CTRL2_ODR_500HZ_2G);
    if (err != ESP_OK) {
        return err;
    }
    /* DRDY stays off INT2 (it is not wired), which suits the Non-SyncSample
     * mode the tap engine needs. */
    err = write_reg(REG_CTRL7, CTRL7_AEN | CTRL7_DRDY_DIS);
    if (err != ESP_OK) {
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "tap engine on: accel 500 Hz, INT1 -> GPIO%d", MUSE_IMU_TAP_GPIO);
    return ESP_OK;
}

bool muse_imu_tap_ready(void)
{
    return s_ready;
}

bool muse_imu_tap_tapped(void)
{
    if (!s_ready) {
        return false;
    }
    uint8_t st = 0;
    if (read_reg(REG_STATUS1, &st) != ESP_OK) {
        return false;
    }
    /* Reading STATUS1 also clears the sticky bit and drops the INT line. */
    return (st & 0x02) != 0;
}

/*
 * QMI8658 tap-to-wake for Waveshare ESP32-S3-Touch-LCD-1.85B.
 *
 * Wiring (from ESP32-S3-Touch-LCD-1.85B Rev1.1 schematic):
 *   QMI8658 INT1 -> R51 (0R) -> GPIO8
 *   QMI8658 INT2 -> NC
 *   QMI8658 I2C addr 0x6B
 *
 * How it works: the QMI8658's on-chip tap engine watches the accelerometer
 * by itself. On a tap it toggles INT1, which wakes the ESP32 from light
 * sleep via a GPIO interrupt. No polling, ~uA in sleep.
 *
 * Register references: QMI8658A Datasheet Rev 1.0
 *   - CTRL9 (0x09) bit0 = Tap_EN, bit6 = ACTIVITY_INT_SEL (1 -> INT1)
 *   - Tap params via CAL1..CAL4 + CTRL9 command 0x0C (CTRL_CMD_CONFIGURE_TAP)
 *   - STATUS1 (0x2F) bit1 = TAP detected (read clears)
 *
 * NOTE: tap threshold tuning is empirical. If taps don't wake or it
 * wakes too easily, adjust QMI_TAP_* below.
 */

#include "qmi8658_tap.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "qmi_tap";

/* QMI8658 registers (QMI8658A datasheet) */
#define QMI_ADDR        0x6B
#define REG_CTRL2       0x02    /* accelerometer config */
#define REG_CTRL7       0x07    /* sensor enable */
#define REG_CTRL8       0x08    /* CTRL8 on some revs == CTRL9 below */
#define REG_CTRL9       0x09    /* bit0 Tap_EN, bit6 ACTIVITY_INT_SEL */
#define REG_CAL1_L      0x0B
#define REG_CAL1_H      0x0C
#define REG_CAL2_L      0x0D
#define REG_CAL2_H      0x0E
#define REG_CAL3_L      0x0F
#define REG_CAL3_H      0x10
#define REG_CAL4_L      0x11
#define REG_CAL4_H      0x12
#define REG_CTRL9_CMD   0x0A    /* CTRL9 host command register */
#define REG_STATUSINT   0x2D
#define REG_STATUS1     0x2F

#define CMD_CONFIGURE_TAP  0x0C

/* Tap tuning (empirical defaults; adjust to taste) */
#define QMI_TAP_QUIET_US   30000    /* quiet time before shock window */
#define QMI_TAP_SHOCK_US   50000    /* shock detection window */

/* Board wiring */
#define QMI_INT_GPIO    GPIO_NUM_8

/* I2C handle: set by qmi8658_tap_init() caller via qmi8658_tap_set_i2c().
 * We reuse the board's existing I2C bus; the caller passes the device
 * handle created for addr 0x6B. */
static void *s_i2c_dev = NULL;

static volatile bool s_tapped = false;

void qmi8658_tap_set_i2c(void *i2c_dev_handle)
{
    s_i2c_dev = i2c_dev_handle;
}

/* --- low-level I2C (ESP-IDF 5.x/6.x i2c_master) ------------------------ */

#include "driver/i2c_master.h"

static esp_err_t qmi_write(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit((i2c_master_dev_handle_t)s_i2c_dev,
                               buf, len + 1, 100);
}

static esp_err_t qmi_write1(uint8_t reg, uint8_t val)
{
    return qmi_write(reg, &val, 1);
}

static esp_err_t qmi_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive((i2c_master_dev_handle_t)s_i2c_dev,
                                       &reg, 1, data, len, 100);
}

/* --- tap engine configuration ----------------------------------------- */
/*
 * Procedure (datasheet §10.3):
 *  1. Disable accel+gyro (CTRL7 = 0x00)
 *  2. Write tap params to CAL1..CAL4
 *  3. Issue CTRL9 command CTRL_CMD_CONFIGURE_TAP (0x0C)
 *  4. Wait for STATUSINT.bit7 (Ctrl9 Cmd Done)
 *  5. Route activity interrupt to INT1 (CTRL9.bit6 = 1)
 *  6. Enable tap engine (CTRL9.bit0 = 1)
 *  7. Enable accelerometer (CTRL7.aEN = 1), ODR >= 200Hz
 */
static esp_err_t qmi_tap_configure(void)
{
    esp_err_t err;
    uint8_t v;

    /* 1. Stop sensors while configuring */
    err = qmi_write1(REG_CTRL7, 0x00);
    if (err != ESP_OK) return err;

    /*
     * 2. Tap parameters. Two command sets are written via CAL1..CAL4;
     *    CAL4_H[7:4] selects the set (0x01 = first, 0x02 = second).
     *    Values below are the datasheet's reference for a firm finger tap.
     */
    uint8_t set1[8] = {
        0x0A,   /* CAL1_L: PeakWindow */
        0x08,   /* CAL1_H: Priority */
        0x3C,   /* CAL2_L: TapWindow L */
        0x00,   /* CAL2_H: TapWindow H */
        0x78,   /* CAL3_L: DTapWindow L / UDMThr */
        0x00,   /* CAL3_H */
        0x00,   /* CAL4_L: NA */
        0x10,   /* CAL4_H: 0x01 in [7:4] = first command set */
    };
    err = qmi_write(REG_CAL1_L, set1, sizeof(set1));
    if (err != ESP_OK) return err;

    /* 3. Issue configure-tap command */
    err = qmi_write1(REG_CTRL9_CMD, CMD_CONFIGURE_TAP);
    if (err != ESP_OK) return err;

    /* 4. Wait for command done (STATUSINT.bit7), up to 100ms */
    for (int i = 0; i < 20; i++) {
        v = 0;
        err = qmi_read(REG_STATUSINT, &v, 1);
        if (err != ESP_OK) return err;
        if (v & 0x80) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!(v & 0x80)) {
        ESP_LOGW(TAG, "tap configure: CTRL9 cmd not done");
        return ESP_FAIL;
    }

    /*
     * 5+6. Route tap interrupt to INT1 and enable tap engine.
     *     CTRL9: bit6 ACTIVITY_INT_SEL=1 -> INT1, bit0 Tap_EN=1.
     */
    err = qmi_write1(REG_CTRL9, (1 << 6) | (1 << 0));
    if (err != ESP_OK) return err;

    /*
     * 7. Accelerometer on, >= 200Hz ODR for tap detection.
     *    CTRL2: aODR=0110 (500Hz), aFS=00 (2g). CTRL7: aEN=1.
     */
    err = qmi_write1(REG_CTRL2, 0x60);
    if (err != ESP_OK) return err;
    err = qmi_write1(REG_CTRL7, 0x01);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "tap engine configured, INT1 -> GPIO%d", QMI_INT_GPIO);
    return ESP_OK;
}

/* --- GPIO interrupt ---------------------------------------------------- */

static void IRAM_ATTR qmi_isr(void *arg)
{
    (void)arg;
    s_tapped = true;
}

esp_err_t qmi8658_tap_init(void)
{
    if (!s_i2c_dev) {
        ESP_LOGE(TAG, "no I2C handle: call qmi8658_tap_set_i2c() first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = qmi_tap_configure();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tap configure failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * INT line TOGGLES on each tap event (not just rising edge),
     * so trigger on any edge. Reading STATUS1 clears the flag and
     * resets the line.
     */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << QMI_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) return err;
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = gpio_isr_handler_add(QMI_INT_GPIO, qmi_isr, NULL);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

bool qmi8658_tap_poll(void)
{
    uint8_t st = 0;
    if (qmi_read(REG_STATUS1, &st, 1) != ESP_OK)
        return false;
    /* bit1 = TAP detected; read clears it */
    return (st & 0x02) != 0;
}

bool qmi8658_tap_consumed(void)
{
    bool t = s_tapped;
    if (t) {
        s_tapped = false;
        /* clear the chip flag too */
        qmi8658_tap_poll();
    }
    return t;
}

esp_err_t qmi8658_tap_sleep(void)
{
    /*
     * Enter light sleep; QMI8658 keeps running on its own power and
     * will toggle INT1 on tap. GPIO8 is an RTC GPIO: wakeup works.
     */
    esp_err_t err = gpio_wakeup_enable(QMI_INT_GPIO, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) return err;
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "light sleep, tap GPIO%d to wake", QMI_INT_GPIO);
    esp_light_sleep_start();
    /* woke up */
    gpio_wakeup_disable(QMI_INT_GPIO);
    ESP_LOGI(TAG, "woke: cause=%d", esp_sleep_get_wakeup_cause());
    return ESP_OK;
}

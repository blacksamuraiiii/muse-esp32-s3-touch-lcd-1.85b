/*
 * Tap-to-wake on the QMI8658A IMU (the 1.85B's 6/9-axis part).
 *
 * The IMU's own tap engine watches the accelerometer; a tap raises INT1, which
 * the board wires to GPIO8 (see board_waveshare_s3_touch_lcd_185b.c). The wake
 * line is merged into the existing button wait (muse_gpio_wake_line_add, called
 * by the board), and the input task also polls STATUS1 every 200 ms while the
 * screen is dark, so a tap whose edge the sleep missed is still caught. There
 * is no dedicated sleep loop of this file's own.
 *
 * Keeping the engine alive is the hard part, not starting it. Two things have
 * killed it in the field, both after a while rather than at boot:
 *
 *  1. A truncated I2C write. Automatic light sleep can stop the APB clock in
 *     the middle of a transaction, and the IMU then parses the half-written
 *     frame as a register write. Every access here runs under an
 *     ESP_PM_NO_LIGHT_SLEEP lock so a transfer is never cut in half.
 *  2. STATUS1 latches. It is cleared only by being read; a tap while the screen
 *     was already on used to leave the bit set, and the engine then stopped
 *     reporting new events. The input task now reads STATUS1 whether it is
 *     asleep or not, and acts on the value only while asleep.
 *
 * On top of that the engine is verified and repaired: muse_imu_tap_check()
 * compares CTRL7/CTRL8 (and, once a minute, CAL1..CAL4) against what init()
 * left there and re-runs init() when they drifted, and the input task
 * re-arms the engine every ten minutes of sleep in case it wedged with its
 * configuration intact (that failure mode leaves no readable trace).
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "muse.imu";

#define REG_CTRL1       0x02    /* serial interface + INT1/INT2 enable */
#define REG_CTRL2       0x03    /* accelerometer ODR / full scale */
#define REG_CTRL7       0x08    /* sensor enable (aEN bit0, DRDY_DIS bit5) */
#define REG_CTRL8       0x09    /* motion engines + INT routing */
#define REG_CTRL9       0x0A    /* host command register */
#define REG_CAL1_L      0x0B    /* tap parameters, see Table 36 */
#define REG_STATUSINT   0x2D    /* bit7: CTRL9 command done, bit1: tap flag */
#define REG_STATUS1     0x2F    /* bit1: tap detected (sticky until read) */
#define REG_TAP_STATUS  0x59    /* which axis / single or double, informational */
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

/* Two re-inits closer together than this would be a loop, not a repair: the
 * chip would have to be losing its configuration faster than it can be
 * rewritten, which a wedged engine or a dead bus explains better. */
#define RECOVER_MIN_MS 10000

static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static SemaphoreHandle_t s_lock;   /* serialises the bus between tasks */

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm;  /* no light sleep while a transfer is in flight */
#endif

/* What init() left in the chip. Drift here means the tap engine is not
 * listening any more, even while the accelerometer keeps running. */
static uint8_t s_exp_ctrl7;
static uint8_t s_exp_ctrl8;
static uint8_t s_exp_cal[8];

static TickType_t s_last_recover;
static unsigned s_recoveries;
static unsigned s_rearms;

/* ---------------------------------------------------------------- bus access */

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
    bus_lock();
    esp_err_t err = bus_write(buf, sizeof(buf));
    bus_unlock();
    if (err != ESP_OK) {
        return err;
    }
    return run_command(CTRL_CMD_CONFIGURE_TAP);
}

/* The parameter set the chip is left holding: set 2's bytes with the set
 * selector in CAL4_H. */
static void remember_params(const uint8_t set2[8])
{
    memcpy(s_exp_cal, set2, 8);
    s_exp_cal[7] = (uint8_t)(2 << 4);
}

/* The body of init, also used to re-arm the engine: the parameters have to go
 * back in with the sensors stopped (datasheet 10.3), so there is no cheaper
 * restart that is still correct. `quiet` keeps the periodic re-arm out of the
 * log at the default level. */
static esp_err_t engine_start(bool quiet)
{
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
    remember_params(set2);

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

    s_exp_ctrl8 = CTRL8_HANDSHAKE_STATUSINT | CTRL8_TAP_TO_INT1 | CTRL8_TAP_EN;
    s_exp_ctrl7 = CTRL7_AEN | CTRL7_DRDY_DIS;
    s_ready = true;
    if (quiet) {
        ESP_LOGD(TAG, "tap engine restarted (#%u)", s_rearms);
    } else {
        ESP_LOGI(TAG, "tap engine on: accel 500 Hz, INT1 -> GPIO%d (CTRL7=0x%02x CTRL8=0x%02x)",
                 MUSE_IMU_TAP_GPIO, s_exp_ctrl7, s_exp_ctrl8);
    }
    return ESP_OK;
}

esp_err_t muse_imu_tap_init(i2c_master_dev_handle_t dev)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
#if CONFIG_PM_ENABLE
    if (!s_pm) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "imu_tap", &s_pm) != ESP_OK) {
            ESP_LOGW(TAG, "no PM lock: a light sleep mid-transfer can still corrupt the IMU");
            s_pm = NULL;
        }
    }
#endif
    s_dev = dev;
    return engine_start(false);
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

/* ------------------------------------------------------- health and recovery */

esp_err_t muse_imu_tap_diag(muse_imu_tap_diag_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    out->ready = s_ready;
    out->recoveries = s_recoveries;
    out->rearms = s_rearms;
    esp_err_t err;
    if ((err = read_reg(REG_CTRL1, &out->ctrl1)) != ESP_OK ||
        (err = read_reg(REG_CTRL2, &out->ctrl2)) != ESP_OK ||
        (err = read_reg(REG_CTRL7, &out->ctrl7)) != ESP_OK ||
        (err = read_reg(REG_CTRL8, &out->ctrl8)) != ESP_OK ||
        (err = read_reg(REG_CTRL9, &out->ctrl9)) != ESP_OK ||
        (err = read_reg(REG_STATUSINT, &out->statusint)) != ESP_OK ||
        (err = read_reg(REG_STATUS1, &out->status1)) != ESP_OK ||
        (err = read_reg(REG_TAP_STATUS, &out->tap_status)) != ESP_OK) {
        return err;
    }
    bus_lock();
    err = bus_read(REG_CAL1_L, out->cal, sizeof(out->cal));
    bus_unlock();
    if (err != ESP_OK) {
        return err;
    }
    out->ctrl7_ok = out->ctrl7 == s_exp_ctrl7;
    out->ctrl8_ok = out->ctrl8 == s_exp_ctrl8;
    out->cal_ok = memcmp(out->cal, s_exp_cal, sizeof(s_exp_cal)) == 0;
    return ESP_OK;
}

void muse_imu_tap_log_diag(const char *why)
{
    muse_imu_tap_diag_t d;
    if (muse_imu_tap_diag(&d) != ESP_OK) {
        ESP_LOGW(TAG, "%s: IMU not readable", why);
        return;
    }
    ESP_LOGW(TAG,
             "%s: CTRL1=0x%02x CTRL2=0x%02x CTRL7=0x%02x(want 0x%02x) "
             "CTRL8=0x%02x(want 0x%02x) CTRL9=0x%02x STATUSINT=0x%02x STATUS1=0x%02x "
             "TAP=0x%02x cal=%02x %02x %02x %02x %02x %02x %02x %02x "
             "recoveries=%u rearms=%u",
             why, d.ctrl1, d.ctrl2, d.ctrl7, s_exp_ctrl7, d.ctrl8, s_exp_ctrl8, d.ctrl9,
             d.statusint, d.status1, d.tap_status,
             d.cal[0], d.cal[1], d.cal[2], d.cal[3], d.cal[4], d.cal[5], d.cal[6], d.cal[7],
             d.recoveries, d.rearms);
}

/* Re-runs init() when the chip no longer holds what init() wrote. Rate-limited:
 * a chip that keeps drifting faster than this needs a hardware look, not a
 * tighter loop. Returns true when the engine is (again) configured. */
static bool recover(const char *why)
{
    TickType_t now = xTaskGetTickCount();
    if (s_last_recover && now - s_last_recover < pdMS_TO_TICKS(RECOVER_MIN_MS)) {
        ESP_LOGW(TAG, "tap config still wrong (%s), waiting before another re-init", why);
        return false;
    }
    s_last_recover = now;
    muse_imu_tap_log_diag(why);
    s_recoveries++;
    if (muse_imu_tap_init(s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "tap re-init failed (#%u)", s_recoveries);
        return false;
    }
    ESP_LOGW(TAG, "tap re-initialised (#%u)", s_recoveries);
    return true;
}

bool muse_imu_tap_check(bool full)
{
    if (!s_ready) {
        return false;
    }
    uint8_t c7 = 0, c8 = 0;
    if (read_reg(REG_CTRL7, &c7) != ESP_OK || read_reg(REG_CTRL8, &c8) != ESP_OK) {
        return false;   /* bus trouble: leave it to the next poll */
    }
    if (c7 != s_exp_ctrl7 || c8 != s_exp_ctrl8) {
        return recover("config drift");
    }
    if (full) {
        uint8_t cal[8] = { 0 };
        bus_lock();
        esp_err_t err = bus_read(REG_CAL1_L, cal, sizeof(cal));
        bus_unlock();
        if (err == ESP_OK && memcmp(cal, s_exp_cal, sizeof(cal)) != 0) {
            return recover("tap parameters lost");
        }
    }
    return true;
}

void muse_imu_tap_rearm(void)
{
    if (!s_dev) {
        return;   /* no IMU was ever found: nothing to restart */
    }
    /* A full restart, not a bare TAP_EN toggle: the parameters have to be
     * written back with the sensors stopped, and a toggle would leave a
     * silently dead engine behind if the chip had dropped them. This is the
     * only repair available for an engine that wedged with its configuration
     * intact, which leaves nothing readable to compare against. */
    s_rearms++;
    s_last_recover = xTaskGetTickCount();
    if (engine_start(true) != ESP_OK) {
        ESP_LOGW(TAG, "tap engine restart failed (#%u)", s_rearms);
    }
}

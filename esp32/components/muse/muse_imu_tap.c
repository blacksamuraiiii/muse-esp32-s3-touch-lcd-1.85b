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
 * compares CTRL1/CTRL2/CTRL7/CTRL8 (and, once a minute, CAL1..CAL4) against
 * what init() left there and re-runs init() when they drifted, and the input
 * task re-arms the engine every ten seconds of sleep in case it wedged with
 * its configuration intact (that failure mode leaves no readable trace).
 * CTRL2 is in that list because it carries the accelerometer's ODR: with the
 * ODR wrong the engine is enabled, its parameters are right, and it receives
 * no samples - the shape of the failures seen on battery.
 *
 * Register names and the parameter tables follow the QMI8658A datasheet
 * (QST, Rev A): CTRL1 0x02, CTRL2 0x03, CTRL7 0x08, CTRL8 0x09, CTRL9 0x0A,
 * CAL1..CAL4 0x0B..0x12, STATUSINT 0x2D, STATUS1 0x2F, TAP_STATUS 0x59.
 * Note the shift against some community snippets: CTRL8 is 0x09, not 0x09's
 * neighbour, and only bits 0-7 of CAL4_H are the command-set selector.
 */

#include "muse_imu_tap.h"

#include <string.h>

#include "esp_attr.h"
#include "muse_state.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
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
#define REG_ACC_X_L     0x35    /* 6 bytes: X, Y, Z, little endian */
#define REG_TAP_STATUS  0x59    /* which axis / single or double, informational */
#define REG_WHO_AM_I    0x00
#define REG_RESET       0x60    /* write RESET_CMD to soft-reset the whole part */
#define RESET_CMD       0xB0

/* 2 g full scale: 16384 LSB per g. */
#define ACC_LSB_PER_G   16384

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
 * Adjust here when a tap on the case is missed or the case is too sensitive.
 *
 * The datasheet's PeakMagThr reference of 800 (0.8 g^2) turned out to be too
 * high for a finger knock on a plastic case resting on a desk: on the bench it
 * woke only sometimes, and the bump log showed the motion was there while
 * STATUS1 stayed clear. 400 (0.4 g^2) with a matching UDMThr is what made a
 * light knock reliable; raise it if the case starts waking from desk noise. */
#define TAP_PEAK_WINDOW_SAMPLES  20    /* 40 ms to reach the peak */
#define TAP_PRIORITY             0     /* only [2:0]; the tap axis, unused here */
#define TAP_WINDOW_SAMPLES       50    /* 100 ms of quiet before a second tap */
#define TAP_DTAP_WINDOW_SAMPLES  250   /* 500 ms to complete a double tap */
#define TAP_ALPHA                8     /* 0.0625: 7-bit fraction of 0.0078 */
#define TAP_GAMMA                32    /* 0.25 */
#define TAP_PEAK_MAG_THR         400   /* 0.4 g^2, units of 0.001 g^2 */
#define TAP_UDM_THR              160   /* 0.16 g, units of 0.001 g */
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
static uint8_t s_exp_ctrl1;
static uint8_t s_exp_ctrl2;
static uint8_t s_exp_ctrl7;
static uint8_t s_exp_ctrl8;
static uint8_t s_exp_cal[8];

static TickType_t s_last_recover;
static unsigned s_recoveries;
static unsigned s_rearms;

/* ------------------------------------------------------------ the tap diary
 *
 * Every failure of tap-to-wake reported so far happened on battery, and a
 * battery run is exactly the one a serial cable cannot take part in: the USB
 * host is what keeps the CPU out of light sleep, and the console dies with it.
 * The diary is the way out: a small record that lives in RTC memory, keeps its
 * contents across a reset as long as the battery stays in, and is printed by
 * the next boot and by ">imu", where a cable is attached again. It answers, in
 * order: did the task ever sleep, did the wake come from the IMU line or from a
 * timer, what did the engine say when it did, and was the engine even
 * configured.
 */
#define DIARY_EVENTS 16
#define DIARY_MAGIC  0x54415045u   /* "TAPE": 字段一改就换，旧的 RTC 内容作废 */

typedef struct {
    uint32_t magic;
    uint32_t boots;
    uint32_t wakes;        /* wait_buttons() returns */
    uint32_t wake_gpio;    /* ... ended by a GPIO wake */
    uint32_t wake_timer;   /* ... ended by a timer wake */
    uint32_t wake_other;   /* ... anything else (a button line, a timeout) */
    uint32_t line_high;    /* ... with the IMU's INT1 up at that moment */
    uint32_t taps;         /* tap bits the poll read while asleep */
    uint32_t taps_awake;   /* tap bits read while the screen was already on */
    uint32_t not_ready;    /* polls that found the engine not configured */
    uint32_t repairs;      /* re-inits the watchdog did */
    uint32_t rearm_fail;   /* re-arms that failed */
    uint32_t starts;       /* engine_start() calls */
    uint32_t knocks;       /* accel-threshold wakes (the engine-independent path) */
    uint32_t drifts;       /* CTRL1/CTRL2/CTRL7/CTRL8 comparisons that did not match */
    int32_t  last_tap_status;
    int32_t  last_int1;
    uint32_t n;            /* events recorded, ever */
    struct { uint32_t tag, ms; int32_t a, b; } ev[DIARY_EVENTS];
} tap_diary_t;

#define DIARY_ENGINE_OK   1
#define DIARY_ENGINE_ERR  2
#define DIARY_REPAIR      3
#define DIARY_REARM       4
#define DIARY_REARM_ERR   5
#define DIARY_WAKE        6
#define DIARY_TAP         7
#define DIARY_TAP_AWAKE   8
#define DIARY_NOT_READY   9
#define DIARY_KNOCK      10
#define DIARY_DRIFT      11

RTC_NOINIT_ATTR static tap_diary_t s_diary;
static bool s_diary_booted;

static void diary_note(uint32_t tag, int32_t a, int32_t b)
{
    s_diary.ev[s_diary.n % DIARY_EVENTS].tag = tag;
    s_diary.ev[s_diary.n % DIARY_EVENTS].ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_diary.ev[s_diary.n % DIARY_EVENTS].a = a;
    s_diary.ev[s_diary.n % DIARY_EVENTS].b = b;
    s_diary.n++;
}

static const char *diary_tag_name(uint32_t tag)
{
    switch (tag) {
    case DIARY_ENGINE_OK:  return "engine-on";
    case DIARY_ENGINE_ERR: return "engine-ERR";
    case DIARY_REPAIR:     return "repaired";
    case DIARY_REARM:      return "rearm";
    case DIARY_REARM_ERR:  return "rearm-ERR";
    case DIARY_WAKE:       return "wake";
    case DIARY_TAP:        return "tap";
    case DIARY_TAP_AWAKE:  return "tap-awake";
    case DIARY_NOT_READY:  return "not-ready";
    case DIARY_KNOCK:      return "knock";
    case DIARY_DRIFT:      return "drift";
    default:               return "?";
    }
}

/* Called once per boot, before the engine is brought up. */
static void diary_boot(void)
{
    if (s_diary.magic != DIARY_MAGIC) {
        memset(&s_diary, 0, sizeof(s_diary));
        s_diary.magic = DIARY_MAGIC;
        s_diary.boots = 1;
        return;
    }
    s_diary.boots++;
}

static void diary_dump(const char *why)
{
    if (s_diary.magic != DIARY_MAGIC) {
        ESP_LOGW(TAG, "tap diary (%s): empty", why);
        return;
    }
    ESP_LOGW(TAG, "tap diary (%s): boots=%u wakes=%u gpio=%u timer=%u other=%u line_high=%u "
             "taps=%u taps_awake=%u knocks=%u not_ready=%u repairs=%u rearm_fail=%u drifts=%u starts=%u",
             why, (unsigned)s_diary.boots, (unsigned)s_diary.wakes, (unsigned)s_diary.wake_gpio,
             (unsigned)s_diary.wake_timer, (unsigned)s_diary.wake_other,
             (unsigned)s_diary.line_high, (unsigned)s_diary.taps,
             (unsigned)s_diary.taps_awake, (unsigned)s_diary.knocks,
             (unsigned)s_diary.not_ready, (unsigned)s_diary.repairs,
             (unsigned)s_diary.rearm_fail, (unsigned)s_diary.drifts,
             (unsigned)s_diary.starts);
    uint32_t n = s_diary.n < DIARY_EVENTS ? s_diary.n : DIARY_EVENTS;
    uint32_t first = s_diary.n - n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (first + i) % DIARY_EVENTS;
        ESP_LOGW(TAG, "  diary[%lu] t=%lums %s a=%ld b=%ld", (unsigned long)(first + i),
                 (unsigned long)s_diary.ev[k].ms, diary_tag_name(s_diary.ev[k].tag),
                 (long)s_diary.ev[k].a, (long)s_diary.ev[k].b);
    }
}

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
 * log at the default level.
 *
 * A failed attempt leaves s_ready as it was. It is cleared on entry because a
 * half-written configuration is not something to trust - but restoring it on
 * failure matters: s_ready false makes muse_imu_tap_tapped() return false
 * unconditionally, so one failed re-arm (a glitch on a bus this congested)
 * would silently kill tap-to-wake until the next reboot. */
static esp_err_t engine_start(bool quiet)
{
    const bool was_ready = s_ready;
    esp_err_t err;
    s_ready = false;
    s_diary.starts++;
    uint8_t who = 0;
    err = read_reg(REG_WHO_AM_I, &who);
    if (err == ESP_OK && who != 0x05) {
        ESP_LOGW(TAG, "IMU WHO_AM_I 0x%02x, expected 0x05", who);
        err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IMU on the bus: %s", esp_err_to_name(err));
        goto out;
    }

    /* Reset the part before configuring it (datasheet Rev A, register 0x60:
     * write 0xB0). Power-on register state is not guaranteed, and a reset
     * clears whatever a previous engine or a half-written frame left behind.
     * 15 ms is the datasheet's figure; 20 ms leaves margin. WHO_AM_I does not
     * change across the reset, so there is nothing to re-check. */
    err = write_reg(REG_RESET, RESET_CMD);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU reset failed: %s", esp_err_to_name(err));
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Auto-increment for burst writes, little-endian, INT1 push-pull enabled,
     * and the CTRL9 handshake kept on STATUSINT so configuring never pulls INT1. */
    err = write_reg(REG_CTRL1, CTRL1_AUTO_INC | CTRL1_INT1_EN);
    if (err != ESP_OK) {
        goto out;
    }
    err = write_reg(REG_CTRL8, CTRL8_HANDSHAKE_STATUSINT);
    if (err != ESP_OK) {
        goto out;
    }
    /* Parameters go in with the sensors stopped (datasheet 10.3). */
    err = write_reg(REG_CTRL7, 0x00);
    if (err != ESP_OK) {
        goto out;
    }

    {
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
            goto out;
        }
        remember_params(set2);
    }

    /* Route the tap event to INT1, enable the engine, then start the
     * accelerometer at 500 Hz (the engine needs the samples). */
    err = write_reg(REG_CTRL8, CTRL8_HANDSHAKE_STATUSINT | CTRL8_TAP_TO_INT1 | CTRL8_TAP_EN);
    if (err != ESP_OK) {
        goto out;
    }
    err = write_reg(REG_CTRL2, CTRL2_ODR_500HZ_2G);
    if (err != ESP_OK) {
        goto out;
    }
    /* DRDY stays off INT2 (it is not wired), which suits the Non-SyncSample
     * mode the tap engine needs. */
    err = write_reg(REG_CTRL7, CTRL7_AEN | CTRL7_DRDY_DIS);
    if (err != ESP_OK) {
        goto out;
    }

    s_exp_ctrl8 = CTRL8_HANDSHAKE_STATUSINT | CTRL8_TAP_TO_INT1 | CTRL8_TAP_EN;
    s_exp_ctrl7 = CTRL7_AEN | CTRL7_DRDY_DIS;
    /* CTRL2 carries the accelerometer's ODR: the tap engine gets no samples
     * without it, and nothing else about the chip looks wrong when it drifts. */
    s_exp_ctrl1 = CTRL1_AUTO_INC | CTRL1_INT1_EN;
    s_exp_ctrl2 = CTRL2_ODR_500HZ_2G;
    s_ready = true;
    diary_note(DIARY_ENGINE_OK, (int32_t)s_exp_ctrl7, (int32_t)s_exp_ctrl8);
    if (quiet) {
        ESP_LOGD(TAG, "tap engine restarted (#%u)", s_rearms);
    } else {
        ESP_LOGI(TAG, "tap engine on: accel 500 Hz, INT1 -> GPIO%d (CTRL7=0x%02x CTRL8=0x%02x)",
                 MUSE_IMU_TAP_GPIO, s_exp_ctrl7, s_exp_ctrl8);
    }
    return ESP_OK;

out:
    s_ready = was_ready;
    diary_note(DIARY_ENGINE_ERR, (int32_t)err, (int32_t)was_ready);
    return err;
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
    if (!s_diary_booted) {
        s_diary_booted = true;
        diary_boot();
        /* The previous session left its record here, and this is the first
         * moment a cable is likely to be attached again. */
        diary_dump("boot");
    }
    s_dev = dev;
    return engine_start(false);
}

bool muse_imu_tap_ready(void)
{
    return s_ready;
}

bool muse_imu_tap_tapped(void)
{
    /* Read even when the engine is not known to be configured: the read is what
     * drops INT1, and a chip that is still running after a half-failed init
     * would otherwise hold the line high for good. */
    uint8_t st = 0;
    if (read_reg(REG_STATUS1, &st) != ESP_OK) {
        return false;
    }
    /* Reading STATUS1 also clears the sticky bit and drops the INT line. */
    bool tapped = (st & 0x02) != 0;
    s_diary.last_tap_status = st;
    if (tapped) {
        /* b: bit0 = the engine was configured, bit1 = the screen was dark.
         * The second bit is what tells a lost wake from a lost tap. */
        int32_t flags = (s_ready ? 1 : 0) | (muse_state_asleep() ? 2 : 0);
        diary_note(DIARY_TAP, (int32_t)st, flags);
    }
    return tapped;
}

/* Motion above gravity, in mG (milli-g), from one accelerometer read. Used by
 * the input task to leave a trace of every knock: printed next to STATUS1, it
 * separates "the engine never saw this knock" from "it saw it and nobody read
 * it". -1 when the bus refuses. */
int muse_imu_tap_bump_mg(void)
{
    if (!s_dev) {
        return -1;
    }
    uint8_t raw[6] = { 0 };
    bus_lock();
    esp_err_t err = bus_read(REG_ACC_X_L, raw, sizeof(raw));
    bus_unlock();
    if (err != ESP_OK) {
        return -1;
    }
    int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
    int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
    int16_t az = (int16_t)(raw[4] | (raw[5] << 8));
    /* Resting on the desk the magnitude is 1 g; anything above that is motion.
     * Squaring avoids the sqrt for the comparison, so squint-free on a CPU
     * that has no hardware divider for doubles. */
    int32_t sq = (int32_t)ax * ax + (int32_t)ay * ay + (int32_t)az * az;
    int32_t rest = (int32_t)ACC_LSB_PER_G * ACC_LSB_PER_G;
    if (sq <= rest) {
        return 0;
    }
    /* Linear around 1 g: the excess in mG, close enough for a log line. */
    int32_t over = sq - rest;
    return (int)(over / (2 * ACC_LSB_PER_G) * 1000 / ACC_LSB_PER_G);
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
             "%s: ready=%d int1=%d CTRL1=0x%02x CTRL2=0x%02x CTRL7=0x%02x(want 0x%02x) "
             "CTRL8=0x%02x(want 0x%02x) CTRL9=0x%02x STATUSINT=0x%02x STATUS1=0x%02x "
             "TAP=0x%02x cal=%02x %02x %02x %02x %02x %02x %02x %02x "
             "recoveries=%u rearms=%u",
             why, (int)s_ready, gpio_get_level(MUSE_IMU_TAP_GPIO),
             d.ctrl1, d.ctrl2, d.ctrl7, s_exp_ctrl7, d.ctrl8, s_exp_ctrl8, d.ctrl9,
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
    s_diary.repairs++;
    diary_note(DIARY_REPAIR, (int32_t)s_recoveries, 0);
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
        /* Not "nothing to check": a boot that failed to configure the engine
         * leaves this false for the rest of the session, which used to mean no
         * watchdog and no tap-to-wake until the next reboot. Repair instead. */
        s_diary.not_ready++;
        diary_note(DIARY_NOT_READY, 0, 0);
        return recover("engine not configured");
    }
    uint8_t c1 = 0, c2 = 0, c7 = 0, c8 = 0;
    if (read_reg(REG_CTRL1, &c1) != ESP_OK || read_reg(REG_CTRL2, &c2) != ESP_OK ||
        read_reg(REG_CTRL7, &c7) != ESP_OK || read_reg(REG_CTRL8, &c8) != ESP_OK) {
        return false;   /* bus trouble: leave it to the next poll */
    }
    if (c1 != s_exp_ctrl1 || c2 != s_exp_ctrl2 || c7 != s_exp_ctrl7 || c8 != s_exp_ctrl8) {
        s_diary.drifts++;
        diary_note(DIARY_DRIFT, ((int32_t)c1 << 24) | ((int32_t)c2 << 16) | ((int32_t)c7 << 8)
                   | (int32_t)c8, ((int32_t)s_exp_ctrl2 << 8) | (int32_t)s_exp_ctrl7);
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
        s_diary.rearm_fail++;
        diary_note(DIARY_REARM_ERR, (int32_t)s_rearms, 0);
        ESP_LOGW(TAG, "tap engine restart failed (#%u)", s_rearms);
    } else {
        diary_note(DIARY_REARM, (int32_t)s_rearms, 0);
    }
}

/* Records one return from wait_buttons(): how long the task was out, what the
 * FreeRTOS tick made of it, and what the IMU's line was doing when it came
 * back. The tick figure next to the wall-clock one settles whether light sleep
 * moves the tick at all on this chip. */
void muse_imu_tap_note_wake(int int1)
{
    s_diary.wakes++;
    s_diary.last_int1 = int1;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        s_diary.wake_gpio++;
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        s_diary.wake_timer++;
    } else {
        s_diary.wake_other++;
    }
    if (int1) {
        s_diary.line_high++;
    }
    diary_note(DIARY_WAKE, int1, (int32_t)cause);
}

void muse_imu_tap_note_knock(int mg)
{
    s_diary.knocks++;
    diary_note(DIARY_KNOCK, mg, 0);
}

void muse_imu_tap_diary_log(const char *why)
{
    diary_dump(why);
}

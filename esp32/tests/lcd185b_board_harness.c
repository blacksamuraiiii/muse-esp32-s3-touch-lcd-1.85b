/* Exercise the production read_power() and round_area() extracted from
 * board_waveshare_s3_touch_lcd_185b.c against scriptable gauge/USB fakes.
 * Modes:
 *   alignment   sweep every single pixel plus three rectangles through round_area
 *   power       one sample per stdin line, one result line per sample:
 *               usb probe attached status operation soc mv ma fail
 *               -> err pct mv charging usb ma temp_c10 warnings infos
 *               fail: 0 none, 1 battery-status read fails, 2 operation-status read fails
 *               (a failed read is ESP_ERR_TIMEOUT; a failed probe is the gauge
 *               not answering at all)
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "bq27220.h"
#include "muse_state.h"

#define BQ27220_ADDR 0x55
static const char *TAG = "board";
static int warnings, infos;

static const char *error_name(esp_err_t err)
{
    (void)err;
    return "error";
}
#define esp_err_to_name(err) error_name(err)
/* To stderr, so the formats get checked against their arguments. */
#define ESP_LOGW(tag, fmt, ...) do { ++warnings; fprintf(stderr, "%s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { ++infos; fprintf(stderr, "%s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)

struct fake_i2c_bus {
    int unused;
};
struct fake_gauge {
    int unused;
};
typedef struct fake_i2c_bus *i2c_master_bus_handle_t;
static struct fake_i2c_bus bus;
static struct fake_gauge gauge;
static i2c_master_bus_handle_t s_i2c = &bus;
static bq27220_handle_t s_gauge = &gauge;

static int probe_ok = 1, usb = 0, fail_which;
static uint16_t status_word = 8, operation_word = 32, soc_v = 57, mv_v = 3876, temp_v = 2982;
static int16_t ma_v;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t handle, uint16_t addr, int timeout_ms)
{
    assert(handle == &bus && addr == BQ27220_ADDR && timeout_ms == 20);
    return probe_ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void muse_i2c_levels(int *sda, int *scl)
{
    *sda = 1;
    *scl = 1;
}

void muse_i2c_report(bool ok)
{
    (void)ok;
}

bool usb_serial_jtag_is_connected(void)
{
    return usb;
}

void check_codecs_after_recovery(void)
{
}

esp_err_t bq27220_get_battery_status(bq27220_handle_t handle, battery_status_t *out)
{
    assert(handle == &gauge);
    if (fail_which == 1) {
        return ESP_ERR_TIMEOUT;
    }
    out->full = status_word;
    return ESP_OK;
}

esp_err_t bq27220_get_operation_status(bq27220_handle_t handle, operation_status_t *out)
{
    assert(handle == &gauge);
    if (fail_which == 2) {
        return ESP_ERR_TIMEOUT;
    }
    uint16_t w = operation_word;
    out->CALMD = (w >> 0) & 1;
    out->SEC = (w >> 1) & 3;
    out->EDV2 = (w >> 3) & 1;
    out->VDQ = (w >> 4) & 1;
    out->INITCOMP = (w >> 5) & 1;
    out->SMTH = (w >> 6) & 1;
    out->BTPINT = (w >> 7) & 1;
    out->RSVD1 = (w >> 8) & 3;
    out->CFGUPDATE = (w >> 10) & 1;
    out->RSVD0 = (w >> 11) & 31;
    return ESP_OK;
}

uint16_t bq27220_get_state_of_charge(bq27220_handle_t handle)
{
    assert(handle == &gauge);
    return soc_v;
}

uint16_t bq27220_get_voltage(bq27220_handle_t handle)
{
    assert(handle == &gauge);
    return mv_v;
}

int16_t bq27220_get_avgcurrent(bq27220_handle_t handle)
{
    assert(handle == &gauge);
    return ma_v;
}

uint16_t bq27220_get_temperature(bq27220_handle_t handle)
{
    assert(handle == &gauge);
    return temp_v;
}

typedef struct {
    int x1, y1, x2, y2;
} lv_area_t;
typedef struct {
    lv_area_t *area;
} lv_event_t;
static void *lv_event_get_param(lv_event_t *event)
{
    return event->area;
}

/* Exact function definitions extracted from the board source by the test. */
#include "lcd185b_board_functions.h"

static void check_area(lv_area_t original)
{
    lv_area_t area = original;
    lv_event_t event = { &area };
    round_area(&event);
    assert((area.x1 & 1) == 0 && (area.y1 & 1) == 0);
    assert((area.x2 & 1) == 1 && (area.y2 & 1) == 1);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 360 && area.y2 < 360);
    assert(area.x1 <= original.x1 && area.y1 <= original.y1);
    assert(area.x2 >= original.x2 && area.y2 >= original.y2);
    assert(original.x1 - area.x1 <= 1 && original.y1 - area.y1 <= 1);
    assert(area.x2 - original.x2 <= 1 && area.y2 - original.y2 <= 1);
    lv_area_t once = area;
    round_area(&event);
    assert(memcmp(&once, &area, sizeof(area)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "alignment") == 0) {
        for (int y = 0; y < 360; ++y) {
            for (int x = 0; x < 360; ++x) {
                check_area((lv_area_t){ x, y, x, y });
            }
        }
        check_area((lv_area_t){ 0, 0, 359, 359 });
        check_area((lv_area_t){ 1, 3, 358, 356 });
        check_area((lv_area_t){ 20, 30, 99, 199 });
        return 0;
    }
    assert(strcmp(argv[1], "power") == 0);
    int f_usb, f_probe, f_attached, f_ma, f_fail;
    unsigned f_status, f_operation, f_soc, f_mv;
    muse_power_t out = { .battery_pct = 99, .battery_mv = 4200, .charging = true, .usb = true };
    while (scanf("%d %d %d %u %u %u %u %d %d", &f_usb, &f_probe, &f_attached, &f_status,
                 &f_operation, &f_soc, &f_mv, &f_ma, &f_fail) == 9) {
        usb = f_usb;
        probe_ok = f_probe;
        s_gauge = f_attached ? &gauge : NULL;
        status_word = (uint16_t)f_status;
        operation_word = (uint16_t)f_operation;
        soc_v = (uint16_t)f_soc;
        mv_v = (uint16_t)f_mv;
        ma_v = (int16_t)f_ma;
        fail_which = f_fail;
        esp_err_t err = read_power(&out);
        printf("%d %d %d %d %d %d %d %d %d\n", err, out.battery_pct, out.battery_mv,
               out.charging, out.usb, out.battery_ma, out.battery_temp_c10, warnings, infos);
    }
    return 0;
}

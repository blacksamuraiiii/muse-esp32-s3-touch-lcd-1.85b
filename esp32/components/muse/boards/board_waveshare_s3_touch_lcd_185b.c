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

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85B: ESP32-S3R8 (16 MB flash, 8 MB octal
 * PSRAM) with a 1.85" 360x360 round TFT on an ST77916 over QSPI, a CST816S
 * capacitive touch controller, an ES8311 codec for the speaker and an ES7210
 * ADC for the dual microphones, a BQ27220 fuel gauge, and native USB.
 *
 * Pin/fact sources (per devices/AGENTS.md, do not guess pins):
 *  - Waveshare official docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B
 *    (LCD QSPI CS=21 PCLK=40 D0=46 D1=45 D2=42 D3=41 RST=3 BL=5;
 *     touch CST816S I2C addr 0x15: SCL=10 SDA=11 RST=1 INT=4;
 *     I2S MCLK=2 BCLK=48 LRCK=38 DOUT=47 DIN=39;
 *     ES8311 I2C 7-bit 0x18, ES7210 I2C 7-bit 0x40 (docs print the 8-bit
 *     forms 0x30/0x80); amp enable PA_CTRL=9;
 *     BOOT=GPIO0; BQ27220 I2C 7-bit 0x55; QMI8658 IMU 0x6B; PCF85063 RTC 0x51)
 *  - Vendor repo (clone it, do not read on the web):
 *    https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B
 *  - Community firmware for this exact board (display/touch/audio/RTC/IMU):
 *    https://github.com/jamesblashill/pocket-watch
 *
 * Strapping pins in use: GPIO0 (BOOT), GPIO3 (LCD RST), GPIO45/46 (LCD
 * D1/D0). GPIO45 selects VDD_SPI at reset: no external pull-ups on the LCD
 * data lines, and the panel must stay high-Z through reset.
 *
 * ST77916 bring-up note: espressif/esp_lcd_st77916 probes register 0x04 to
 * pick one of two init tables; on some ST77916 panels it picks the wrong one
 * (fine stripes / edge color bands). If the picture looks wrong, pass a
 * custom init table via st77916_vendor_config_t.init_cmds instead of the
 * default (see the component's README and
 * https://github.com/chuck3cz/ws-knob-meteoplaneradar for a panel that
 * needed it).
 *
 * Self-interruption note: Muse records only while the TALK button is held;
 * there is no always-on VAD and no full-duplex voice path, so the speaker
 * can never false-trigger recording the way it does on always-listening
 * firmware. Pressing TALK mid-reply (barge-in) first discards
 * CONFIG_MUSE_BARGE_IN_SETTLE_MS of capture (muse_voice.c).
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_H_RES 360
#define LCD_V_RES 360
#define LCD_HOST SPI3_HOST
#define LCD_CS GPIO_NUM_21
#define LCD_PCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_RST GPIO_NUM_3
#define LCD_BL GPIO_NUM_5       /* LEDC PWM dimming */
/* Two 32-line buffers in PSRAM: 360*32*2*2 = 46 KB. Internal RAM stays free
 * for Wi-Fi/BLE. */
#define DRAW_BUF_LINES 32

#define TP_ADDR 0x15           /* CST816S, 7-bit */
#define TP_RST GPIO_NUM_1
#define TP_INT GPIO_NUM_4

#define I2C_SDA GPIO_NUM_11    /* touch, ES8311, ES7210, BQ27220, IMU, RTC */
#define I2C_SCL GPIO_NUM_10

#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47   /* playback -> ES8311 */
#define I2S_DIN GPIO_NUM_39    /* record <- ES7210 */
#define PA_EN GPIO_NUM_9       /* amp on while HIGH */

#define BQ27220_ADDR 0x55      /* fuel gauge, 7-bit */

#define TALK_GPIO GPIO_NUM_0   /* BOOT; the PWR key is hardware-only */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3,
        LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t));
    if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io) != ESP_OK) {
        return NULL;
    }
    /* use_qspi_interface=1; init_cmds left NULL for the component default.
     * See the ST77916 bring-up note at the top if the picture is wrong. */
    const st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    if (esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* If red and blue are swapped on first boot, flip rgb_ele_order to BGR. */
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }

    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.dev_addr = TP_ADDR;
    tp_io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tp_io;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TP_RST,
        .int_gpio_num = TP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* ES8311 out and ES7210 in on one duplex I2S bus. The ES7210's second
 * channel carries the speaker reference (xiaozhi uses it for echo
 * cancelling); Muse takes slot 0 (MIC1). */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk }, 1, timeout_ms);
}

static esp_err_t bq27220_read_word(uint8_t reg, uint16_t *out)
{
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_i2c, BQ27220_ADDR, &reg, 1, data, 2, 50),
        TAG, "bq27220 read");
    *out = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

/* BQ27220 fuel gauge: Voltage() 0x08 and StateOfCharge() 0x1C are stable
 * across the whole bq27xxx family. Charging state isn't decoded here. */
static esp_err_t read_power(muse_power_t *out)
{
    if (i2c_master_probe(s_i2c, BQ27220_ADDR, 20) != ESP_OK) {
        ESP_LOGW(TAG, "BQ27220 not answering at 0x%02x", BQ27220_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t soc, mv;
    ESP_RETURN_ON_ERROR(bq27220_read_word(0x1c, &soc), TAG, "state of charge");
    ESP_RETURN_ON_ERROR(bq27220_read_word(0x08, &mv), TAG, "voltage");
    out->battery_pct = soc <= 100 ? (int)soc : 100;
    out->battery_mv = (int)mv;
    out->charging = false;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "wake button");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .aux_button = "boot",   /* single button board, like the Guition: aux never fires */
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -10 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* MIC1; slot 1 is the speaker reference */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}

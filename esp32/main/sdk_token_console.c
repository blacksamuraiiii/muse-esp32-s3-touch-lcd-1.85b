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

#include "sdk_token_console.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "identity.h"
#include "wifi_mgr.h"

// The chip's own USB port, where it has one and the log goes to it.
#define USJ_CONSOLE (SOC_USB_SERIAL_JTAG_SUPPORTED && \
                     (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG))
#if USJ_CONSOLE
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "link.sdk_token";

#define COMMAND "sdk_token="
// A command line: '>', the command and a token, with room to spare.
#define COMMAND_LINE_MAX 96

// "key=value" inside a command line, value ends at the next space.
static bool kv(const char *line, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if (p > line && p[-1] != ' ') {   // a suffix of another word
            p += klen;
            continue;
        }
        if (p[klen] != '=') {
            p += klen;
            continue;
        }
        p += klen + 1;
        size_t n = 0;
        while (p[n] && p[n] != ' ' && n < cap - 1) {
            out[n] = p[n];
            n++;
        }
        out[n] = '\0';
        return n > 0;
    }
    return false;
}

// Network commands. The address lives in NVS so a release built with the
// CONFIG_HOMEHUB_STATIC_* strings empty still leaves a configured device on
// its address; >net.clear hands the station back to DHCP.
static bool net_console_command(const char *line) {
    if (!strcmp(line, "net")) {
        wifi_mgr_netcfg_t cfg;
        wifi_mgr_netcfg(&cfg);
        esp_netif_t *netif = wifi_mgr_get_netif();
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_dns_info_t dns = { 0 };
        esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
        if (netif) {
            esp_netif_get_ip_info(netif, &ip);
            esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
            esp_netif_dhcpc_get_status(netif, &st);
        }
        printf("@net ip " IPSTR " mask " IPSTR " gw " IPSTR,
               IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
        if (dns.ip.type == ESP_IPADDR_TYPE_V4) {
            printf(" dns " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        printf(" dhcpc=%s src=%s cfg=\"%s/%s gw %s dns %s\"\n",
               st == ESP_NETIF_DHCP_STARTED ? "started"
                                            : st == ESP_NETIF_DHCP_STOPPED ? "stopped" : "init",
               !cfg.ip[0] ? "dhcp" : cfg.from_nvs ? "nvs" : "build",
               cfg.ip, cfg.mask, cfg.gw, cfg.dns[0] ? cfg.dns : "-");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "net.set", 7)) {
        char ip[16] = { 0 }, mask[16] = { 0 }, gw[16] = { 0 }, dns[16] = { 0 };
        if (!kv(line, "ip", ip, sizeof(ip)) || !kv(line, "mask", mask, sizeof(mask))
            || !kv(line, "gw", gw, sizeof(gw))) {
            printf("@net.set error usage: >net.set ip=A mask=M gw=G [dns=D]\n");
            fflush(stdout);
            return true;
        }
        kv(line, "dns", dns, sizeof(dns));   // optional
        if (!wifi_mgr_netcfg_save(ip, mask, gw, dns)) {
            printf("@net.set error invalid address\n");
            fflush(stdout);
            return true;
        }
        printf("@net.set ok %s/%s gw %s dns %s, restarting\n", ip, mask, gw, dns[0] ? dns : "-");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));   // let the answer reach the host
        esp_restart();
        return true;
    }
    if (!strcmp(line, "net.clear")) {
        bool ok = wifi_mgr_netcfg_clear();
        printf("@net.clear %s, restarting\n", ok ? "ok" : "failed");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return true;
    }
    return false;
}

bool sdk_token_console_command(const char *line) {
    if (net_console_command(line)) return true;
    if (strncmp(line, COMMAND, strlen(COMMAND)) != 0) return false;
    const char *token = line + strlen(COMMAND);
    if (!identity_sdk_token_valid(token)) {
        ESP_LOGW(TAG, "not an SDK token");
        printf("@sdk_token error invalid\n");
        fflush(stdout);
        return true;
    }
    if (!identity_set_sdk_token(token)) {
        ESP_LOGE(TAG, "couldn't store the SDK token");
        printf("@sdk_token error storage\n");
        fflush(stdout);
        return true;
    }
    ESP_LOGI(TAG, "stored SDK token %.12s, restarting to use it", token);
    printf("@sdk_token ok\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));   // let the answer reach the host
    esp_restart();
    return true;
}

// Collects bytes into a line and runs it on '\n' if it starts with '>'.
// Longer lines are dropped.
typedef struct {
    char buf[COMMAND_LINE_MAX];
    size_t len;
    bool overflow;
} line_reader_t;

static void feed(line_reader_t *r, char c) {
    if (c == '\r') return;
    if (c != '\n') {
        if (r->len < sizeof(r->buf) - 1) {
            r->buf[r->len++] = c;
        } else {
            r->overflow = true;
        }
        return;
    }
    r->buf[r->len] = '\0';
    if (!r->overflow && r->buf[0] == '>') sdk_token_console_command(r->buf + 1);
    r->len = 0;
    r->overflow = false;
}

#if CONFIG_ESP_CONSOLE_UART
static void uart_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

#if USJ_CONSOLE
static void usj_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

void sdk_token_console_start(void) {
#if CONFIG_ESP_CONSOLE_UART
    // With the driver installed, the log goes through it too, so it can't
    // interleave with what the driver sends.
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        xTaskCreate(uart_task, "sdk_token_uart", 3072, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no console UART; can't set the SDK token over it");
    }
#endif
#if USJ_CONSOLE
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        xTaskCreate(usj_task, "sdk_token_usj", 3072, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no USB Serial/JTAG driver; can't set the SDK token over it");
    }
#endif
}

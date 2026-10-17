/*
 * GitHub Release OTA: fetch latest.json, compare versions, hand the image URL
 * to ota_start() (which does the streaming install, signature check and
 * reboot). See ota_github.h for the manifest shape.
 *
 * The repository has to be public: the device pulls the release asset without
 * a token. GitHub answers releases/latest/download/<file> with a 302 to
 * objects.githubusercontent.com, so redirects must be followed.
 */

#include "ota_github.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "ota.h"

static const char *TAG = "ota.github";

/* Override with -DOTA_GH_MANIFEST_URL=... when forking under another account. */
#ifndef OTA_GH_MANIFEST_URL
#define OTA_GH_MANIFEST_URL \
    "https://github.com/blacksamuraiiii/muse-esp32-s3-touch-lcd-1.85b/releases/latest/download/latest.json"
#endif

#define OTA_GH_BODY_MAX 4096
#define OTA_GH_TASK_STACK 12288
#define OTA_GH_TASK_PRIO 4
#define OTA_GH_TIMEOUT_MS 15000
#define OTA_GH_MAX_REDIRECTS 5
#define OTA_GH_TX_BUF 2048

typedef enum {
    GH_IDLE,        /* nothing checked yet this boot */
    GH_CHECKING,    /* manifest fetch in flight */
    GH_LATEST,      /* manifest read; running image is current */
    GH_DOWNLOADING, /* handed to ota_start(); reboots on success */
    GH_FAILED,
} gh_state_t;

static volatile gh_state_t s_state;
static char s_detail[80] = "";      /* what to show next to the state */
static char s_latest[24] = "";      /* version named by the manifest */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} body_t;

static esp_err_t on_manifest_data(esp_http_client_event_t *ev)
{
    body_t *b = (body_t *)ev->user_data;
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0 || !b) {
        return ESP_OK;
    }
    size_t room = b->cap - b->len - 1;
    size_t n = (size_t)ev->data_len < room ? (size_t)ev->data_len : room;
    if (n) {
        memcpy(b->buf + b->len, ev->data, n);
        b->len += n;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

/* "1.1" / "v999.1.6" -> {1,1,0} / {999,1,6}. Non-numeric prefixes are skipped,
 * so a tag like "v1.2" compares the same as "1.2". */
static void parse_ver(const char *s, long out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!s) {
        return;
    }
    const char *p = s;
    for (int part = 0; part < 3; part++) {
        while (*p && (*p < '0' || *p > '9')) {
            p++;
        }
        if (!*p) {
            return;
        }
        char *end = NULL;
        out[part] = strtol(p, &end, 10);
        if (end == p) {
            return;
        }
        p = end;
    }
}

static bool version_is_newer(const char *cand, const char *cur)
{
    long a[3], b[3];
    parse_ver(cand, a);
    parse_ver(cur, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

static void set_state(gh_state_t st, const char *detail)
{
    strlcpy(s_detail, detail ? detail : "", sizeof(s_detail));
    s_state = st;
}

static void on_ota_event(const ota_event_t *ev, void *user)
{
    (void)user;
    switch (ev->result) {
    case OTA_RESULT_APPLIED:
        /* The task reboots shortly; leave the row showing what happened. */
        set_state(GH_DOWNLOADING, "Restarting");
        break;
    case OTA_RESULT_SKIPPED:
        set_state(GH_LATEST, ev->detail ? ev->detail : "already current");
        break;
    case OTA_RESULT_FAILED:
    default: {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s", ev->detail ? ev->detail : "download failed");
        set_state(GH_FAILED, msg);
        break;
    }
    }
}

/* Reads the manifest. Returns true and fills version/url (both caller-freed
 * on success) when the body parses. */
static bool read_manifest(char **version_out, char **url_out)
{
    char *body = (char *)malloc(OTA_GH_BODY_MAX);
    if (!body) {
        set_state(GH_FAILED, "out of memory");
        return false;
    }
    body[0] = '\0';
    body_t b = { .buf = body, .len = 0, .cap = OTA_GH_BODY_MAX };

    const esp_http_client_config_t cfg = {
        .url = OTA_GH_MANIFEST_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_GH_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = OTA_GH_TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = OTA_GH_MAX_REDIRECTS,
        .keep_alive_enable = true,
        .event_handler = on_manifest_data,
        .user_data = &b,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(body);
        set_state(GH_FAILED, "http client init failed");
        return false;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manifest fetch failed: %s", esp_err_to_name(err));
        char msg[64];
        snprintf(msg, sizeof(msg), "manifest: %s", esp_err_to_name(err));
        set_state(GH_FAILED, msg);
        free(body);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "manifest answered %d", status);
        char msg[48];
        snprintf(msg, sizeof(msg), "manifest HTTP %d", status);
        set_state(GH_FAILED, msg);
        free(body);
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "manifest is not JSON");
        set_state(GH_FAILED, "manifest not JSON");
        return false;
    }
    const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(jver) || !jver->valuestring || !cJSON_IsString(jurl) || !jurl->valuestring) {
        ESP_LOGW(TAG, "manifest lacks version/url");
        set_state(GH_FAILED, "manifest lacks version/url");
        cJSON_Delete(root);
        return false;
    }
    ESP_LOGI(TAG, "manifest: version=%s", jver->valuestring);
    *version_out = strdup(jver->valuestring);
    *url_out = strdup(jurl->valuestring);
    cJSON_Delete(root);
    if (!*version_out || !*url_out) {
        free(*version_out);
        free(*url_out);
        set_state(GH_FAILED, "out of memory");
        return false;
    }
    return true;
}

static void check_task(void *arg)
{
    (void)arg;
    char running[sizeof(esp_app_get_description()->version) + 1];
    memcpy(running, esp_app_get_description()->version, sizeof(running) - 1);
    running[sizeof(running) - 1] = '\0';

    set_state(GH_CHECKING, "Checking GitHub");

    char *version = NULL, *url = NULL;
    if (!read_manifest(&version, &url)) {
        goto done;
    }
    strlcpy(s_latest, version, sizeof(s_latest));

    if (!version_is_newer(version, running)) {
        ESP_LOGI(TAG, "up to date (running %s, latest %s)", running, version);
        char msg[64];
        snprintf(msg, sizeof(msg), "latest %s", version);
        set_state(GH_LATEST, msg);
        goto done;
    }

    ESP_LOGI(TAG, "updating %s -> %s", running, version);
    char msg[64];
    snprintf(msg, sizeof(msg), "Installing %s", version);
    set_state(GH_DOWNLOADING, msg);
    /* ota_start() applies its own numeric gate and verifies the image
     * (SHA-256 plus the signature this build signs with). */
    ota_start(url, false, on_ota_event, NULL);

done:
    free(version);
    free(url);
    vTaskDelete(NULL);
}

bool ota_github_check(void)
{
    gh_state_t st = s_state;
    if (st == GH_CHECKING || st == GH_DOWNLOADING) {
        return false;
    }
    set_state(GH_CHECKING, "Checking GitHub");
    /* 12 KB of stack: TLS against github.com, then esp_https_ota reuses the
     * heap for the install. Internal RAM first (mbedtls is happiest there),
     * PSRAM as the fallback when the chat session is holding the heap. */
    if (xTaskCreate(check_task, "ota_gh", OTA_GH_TASK_STACK, NULL, OTA_GH_TASK_PRIO, NULL) == pdPASS) {
        return true;
    }
    if (xTaskCreateWithCaps(check_task, "ota_gh", OTA_GH_TASK_STACK, NULL, OTA_GH_TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) == pdPASS) {
        return true;
    }
    ESP_LOGE(TAG, "no task for the manifest check");
    set_state(GH_FAILED, "no task for the check");
    return false;
}

bool ota_github_busy(void)
{
    gh_state_t st = s_state;
    return st == GH_CHECKING || st == GH_DOWNLOADING;
}

void ota_github_status(char *buf, size_t n)
{
    if (!buf || !n) {
        return;
    }
    int pct = ota_progress_pct();
    switch (s_state) {
    case GH_IDLE:
        strlcpy(buf, s_latest[0] ? s_latest : "Tap to check", n);
        break;
    case GH_CHECKING:
        strlcpy(buf, "Checking...", n);
        break;
    case GH_LATEST:
        strlcpy(buf, s_detail[0] ? s_detail : "Up to date", n);
        break;
    case GH_DOWNLOADING:
        if (pct >= 0 && pct <= 100) {
            snprintf(buf, n, "%s %d%%", s_detail[0] ? s_detail : "Downloading", pct);
        } else {
            strlcpy(buf, s_detail[0] ? s_detail : "Downloading", n);
        }
        break;
    case GH_FAILED:
    default:
        snprintf(buf, n, "Failed: %s", s_detail);
        break;
    }
}

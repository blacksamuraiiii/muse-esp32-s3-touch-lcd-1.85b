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

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_sentence.h"
#include "muse_settings.h"
#include "muse_wifi.h"   /* muse_wifi_connected(): the probe needs a network */

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "muse.tts";

/* Sized to cover the gap between one HTTP read and the next turn iteration,
 * not the whole reply: the chat task moves audio into the turn's own buffer as
 * fast as it goes out of the speaker. */
#define TTS_RING_BYTES 16384
#define TTS_HTTP_BUF 1024
/* The request line must fit the client's TX buffer, whose default is only 512
 * bytes (DEFAULT_HTTP_BUF_SIZE): percent-encoding turns one 3-byte Chinese
 * character into 9 characters, so a 180-byte chunk of Chinese needs ~540 for
 * the text plus ~53 of method/path/version. 512 overflows -> "Out of buffer"
 * from esp_http_client and the chunk is lost (ESP_FAIL). */
#define TTS_HTTP_TX_BUF 2048
/* Per request. Short enough that any proxy in the path is happy with the URL,
 * and that playback starts before a long reply has finished synthesising. */
#define TTS_TEXT_CHUNK MUSE_SENTENCE_CHUNK
/* The TLS handshake runs on this task, and mbedtls wants more than the 8 KB
 * the endpoint itself needs; a short stack shows up as an unexplained
 * "TTS request failed". */
#define TTS_TASK_STACK 12288
#define TTS_TASK_PRIO 4
#define TTS_SEND_TIMEOUT_MS 3000
#define TTS_HTTP_TIMEOUT_MS 8000
/* The reply's first request is the one the listener is waiting for: give up
 * on it this soon instead, so a stalled connect or a service that has not
 * started answering costs three seconds before the retry, not eight. Once
 * the headers are in, the rest of the body gets the full timeout again. */
#define TTS_HTTP_FIRST_MS 3000
#define TTS_RETRY_DELAY_MS 150

static StreamBufferHandle_t s_ring;   /* storage in PSRAM: internal RAM is the scarce one */
static TaskHandle_t s_task;           /* one long-lived worker; its stack lives in PSRAM */
static volatile bool s_fetch_done;    /* the worker has finished every request */
static volatile bool s_abort;
/* Bumped by every muse_tts_speak() and muse_tts_cancel(). A fetch remembers the
 * generation it started in; once that no longer matches, its bytes are dropped
 * instead of being pushed into the ring of whatever is playing now. */
static volatile uint32_t s_gen;
static volatile uint32_t s_fetch_gen;
/* C2: the unit being fetched is the message's last (set by speak/enqueue,
 * read by fetch_reply), and the most recent completed fetch carried the
 * bridge's last-chunk marker. */
static volatile bool s_last_unit;
static volatile bool s_last_chunk;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;  /* s_pending/s_busy handoff */
static volatile bool s_busy;          /* a reply is being fetched right now */
static bool s_short_wait;            /* this fetch started on TTS_HTTP_FIRST_MS */
static bool s_reply_first;           /* the next fetch_reply() is a reply's first */
static char *volatile s_pending;      /* text handed to the worker by muse_tts_speak */
static volatile bool s_ring_dirty;    /* a reset failed on a worker blocked in ring_send:
                                         stale bytes of an older reply are in the ring */
static volatile size_t s_bytes;       /* bytes of the request being read */
static volatile int64_t s_first_byte_us; /* when the first body byte arrived */

/* Strict payload gate. The decoder plays noise out of anything it is handed, so
 * an error page (HTML) or a truncated body must never reach the ring. The first
 * bytes are held back until they look like MPEG audio: an MPEG frame sync
 * (0xFF) or an ID3v2 tag. Matching only 0xFF would reject plenty of valid
 * ID3-prefixed files. */
/* Big enough for the assembled URL: scheme + host/path + key + voice + rate +
 * one chunk of text (muse_tts.c's tts_template_into()). */
#define TTS_TEMPLATE_CAP (MUSE_SPEECH_URL_MAX + MUSE_SPEECH_KEY_MAX + MUSE_SPEECH_VOICE_MAX + \
                          MUSE_SPEECH_RATE_MAX + 24)

#define TTS_PREFIX_BYTES 8
static uint8_t s_prefix[TTS_PREFIX_BYTES];
static size_t s_prefix_n;
static bool s_payload_open;   /* the prefix passed: bytes go to the ring now */
static bool s_payload_bad;    /* the prefix failed: drop the rest, do not retry */

static esp_http_client_handle_t s_client;
static volatile bool s_prewarm;          /* the connection probe is running: ignore its body */
static TaskHandle_t s_prewarm_task;
static int64_t s_client_last_us;         /* when the last reply request finished */
/* An idle keep-alive socket older than this is dropped by prewarm: servers and
 * NAT boxes reap idle connections, and reconnecting while the model is still
 * writing is free. */
#define TTS_IDLE_DROP_US (45 * 1000 * 1000)
#define TTS_PREWARM_TIMEOUT_MS 6000
#define TTS_PREWARM_STACK 8192

static bool mp3_prefix_ok(const uint8_t *p, size_t n)
{
    if (n < 4) {
        return false;
    }
    if (p[0] == 0xFF) {
        return true;                            /* MPEG frame sync */
    }
    return memcmp(p, "ID3", 3) == 0;            /* ID3v2 tag */
}

static size_t url_encode(char *out, size_t cap, const char *in, size_t in_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9')
                     || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            if (n + 1 >= cap) return 0;
            out[n++] = (char)c;
        } else {
            if (n + 3 >= cap) return 0;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 0xf];
        }
    }
    out[n] = '\0';
    return n;
}

static size_t chunk_len(const char *p, size_t left)
{
    if (left <= TTS_TEXT_CHUNK) return left;
    size_t cut = TTS_TEXT_CHUNK;
    while (cut > 1 && ((unsigned char)p[cut] & 0xC0) == 0x80) cut--;
    for (size_t i = cut; i > cut / 2; i--) {
        if (muse_sentence_break_before(p, i)) return i;
    }
    return cut;
}

/* Bytes on their way to the decoder. False when nobody is draining. */
static bool ring_send(const uint8_t *d, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t n = xStreamBufferSend(s_ring, d + off, len - off, pdMS_TO_TICKS(TTS_SEND_TIMEOUT_MS));
        if (n == 0) {
            /* Nobody is draining: the turn moved on. Stop rather than wedge a
             * socket, and let the task wind down. Only flag abort while this
             * fetch is still the current generation: after a barge-in this
             * timeout can fire well into the next reply, and an unconditional
             * s_abort here would silence every byte of it. */
            if (s_fetch_gen == s_gen) {
                ESP_LOGW(TAG, "audio not being read, stopping the fetch");
                s_abort = true;
            }
            return false;
        }
        off += n;
    }
    return true;
}

/* Holds the first TTS_PREFIX_BYTES back until they look like MPEG audio, then
 * streams everything through. A failing prefix marks the whole reply bad so the
 * rest is dropped and the caller does not retry. */
static void gate_feed(const uint8_t *d, size_t len)
{
    size_t off = 0;
    if (!s_payload_open) {
        size_t room = TTS_PREFIX_BYTES - s_prefix_n;
        size_t take = len < room ? len : room;
        memcpy(s_prefix + s_prefix_n, d, take);
        s_prefix_n += take;
        off = take;
        if (s_prefix_n < TTS_PREFIX_BYTES) {
            return;                 /* not enough to judge yet */
        }
        if (!mp3_prefix_ok(s_prefix, s_prefix_n)) {
            ESP_LOGW(TAG, "muse.tts: bad MP3 payload len=%u first=0x%02x", (unsigned)s_bytes, s_prefix[0]);
            s_payload_bad = true;
            if (s_fetch_gen == s_gen) {
                s_abort = true;
            }
            return;
        }
        s_payload_open = true;
        if (!ring_send(s_prefix, TTS_PREFIX_BYTES)) {
            return;
        }
    }
    if (off < len && !s_abort) {
        ring_send(d + off, len - off);
    }
}

static esp_err_t on_http_data(esp_http_client_event_t *ev)
{
    if (s_prewarm) {
        return ESP_OK;   /* the probe only wants the connection, not the body */
    }
    if (ev->event_id == HTTP_EVENT_ON_HEADER || ev->event_id == HTTP_EVENT_ON_DATA) {
        if (s_short_wait && ev->client) {
            /* The wait that matters is over: the body may stream for as long as
             * synthesising it takes. */
            s_short_wait = false;
            esp_http_client_set_timeout_ms(ev->client, TTS_HTTP_TIMEOUT_MS);
        }
        if (ev->event_id == HTTP_EVENT_ON_HEADER) {
            return ESP_OK;
        }
    }
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0) {
        return ESP_OK;
    }
    /* Cancelled (barge-in) or taken over by a newer reply: drop these bytes.
     * Returning an error here would not help - esp_http_client's http_on_body()
     * ignores what the handler returns - so the socket is left to drain on its
     * own, in the background, while the next turn speaks over it. */
    if (s_abort || s_fetch_gen != s_gen) {
        return ESP_OK;
    }
    if (s_first_byte_us == 0) {
        s_first_byte_us = esp_timer_get_time();
    }
    s_bytes += (size_t)ev->data_len;
    gate_feed((const uint8_t *)ev->data, (size_t)ev->data_len);
    return ESP_OK;
}

/* A body shorter than the gate: judge it on what arrived and be done. */
static void gate_finish(void)
{
    if (s_abort || s_fetch_gen != s_gen) {
        return;
    }
    if (s_payload_open || s_payload_bad || !s_prefix_n) {
        return;
    }
    if (mp3_prefix_ok(s_prefix, s_prefix_n)) {
        s_payload_open = true;
        ring_send(s_prefix, s_prefix_n);
    } else {
        ESP_LOGW(TAG, "muse.tts: bad MP3 payload len=%u first=0x%02x", (unsigned)s_bytes, s_prefix[0]);
        s_payload_bad = true;
    }
}

/* The reply connection, kept between requests. A fresh TLS handshake costs
 * 3.5-4.3 s on this board (measured 2026-11-10 on the word audio: the server
 * answers in 18 ms once the socket is up), and a reply is several requests, so
 * a client per request paid that cost per sentence. One handle with keep-alive
 * pays one handshake per turn -- and muse_tts_prewarm() pays that one while the
 * model is still writing, before any text has arrived. */

static bool ensure_client(const char *url, int timeout_ms)
{
    if (s_client) {
        esp_http_client_set_timeout_ms(s_client, timeout_ms);
        esp_http_client_set_url(s_client, url);
        return true;
    }
    /* Some TTS endpoints refuse a request with no browser User-Agent. */
    const esp_app_desc_t *app = esp_app_get_description();
    static char user_agent[48];
    snprintf(user_agent, sizeof(user_agent), "MuseGadget/%s", app->version);
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .buffer_size = TTS_HTTP_BUF,
        .buffer_size_tx = TTS_HTTP_TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = user_agent,
        .event_handler = on_http_data,
        .keep_alive_enable = true,
    };
    s_client = esp_http_client_init(&cfg);
    if (!s_client) {
        ESP_LOGW(TAG, "http client init failed");
        return false;
    }
    return true;
}

/* A socket that failed mid-body, was aborted, or answered with something that
 * is not our audio is suspect: the next request gets a new connection rather
 * than the remains of this one. */
static void drop_client(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

/* THE ROOT URL (scheme://host[:port]/) of the configured endpoint: enough for
 * the prewarm to complete TCP+TLS and leave the socket in the keep-alive pool.
 * No text, so no synthesis and no quota spent. */
static const char *tts_template_into(char *buf, size_t cap);

static bool tts_root_url(char *out, size_t cap)
{
    char tpl[TTS_TEMPLATE_CAP];
    const char *t = tts_template_into(tpl, sizeof(tpl));
    const char *sep = t ? strstr(t, "://") : NULL;
    if (!sep) {
        return false;
    }
    const char *host = sep + 3;
    const char *slash = strchr(host, '/');
    size_t n = slash ? (size_t)(slash - t) + 1 : strlen(t);
    if (n == 0 || n + 1 > cap) {
        return false;
    }
    memcpy(out, t, n);
    out[n] = '\0';
    return true;
}

static bool fetch_once(const char *url, int timeout_ms)
{
    if (!ensure_client(url, timeout_ms)) {
        return false;
    }
    esp_http_client_handle_t c = s_client;
    s_bytes = 0;
    s_first_byte_us = 0;
    s_prefix_n = 0;
    s_payload_open = false;
    s_payload_bad = false;
    s_last_chunk = false;   /* set below, only when this response carries it */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int64_t body = esp_http_client_get_content_length(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS request failed: %s (url %u bytes, budget %u)",
                 esp_err_to_name(err), (unsigned)strlen(url),
                 (unsigned)TTS_HTTP_TX_BUF);
        drop_client();   /* the retry starts from a new connection */
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "TTS endpoint answered %d", status);
        drop_client();
        return false;
    }
    /* C2: the bridge marks the last chunk of the reply (X-TTS-Last: 1) when
     * the request carried &last=1; an old bridge sends no such header. Only a
     * fetch of the current generation counts: a stale fetch that finished
     * after a new turn started must not mark the new turn's audio. */
    char last_hdr[8];
    size_t last_len = sizeof(last_hdr);
    if (s_fetch_gen == s_gen && !s_abort
        && esp_http_client_get_header(c, "X-TTS-Last", last_hdr, &last_len) == ESP_OK
        && last_hdr[0] == '1') {
        s_last_chunk = true;
    }
    gate_finish();
    if (s_payload_bad || s_abort) {
        /* Nothing went to the decoder, or the read was abandoned midway: the
         * socket may hold an unread body, so it must not be reused. */
        drop_client();
        /* Nothing went to the decoder: the reply stays on screen as text. */
        return false;
    }
    /* A streamed reply has no Content-Length (-1), so report what we read and
     * how long the first byte took: that is the number the stream change moves. */
    s_client_last_us = esp_timer_get_time();
    ESP_LOGI(TAG, "TTS got %u bytes (content-length %lld, first byte %d ms, total %d ms)",
             (unsigned)s_bytes, (long long)body,
             s_first_byte_us ? (int)((s_first_byte_us - t0) / 1000) : -1,
             (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/* One retry, but only when nothing reached the decoder. A 502 carries an error
 * page that counts as bytes yet never opened the payload, so it is retried;
 * re-requesting after a partial stream instead would feed the decoder the same
 * audio twice. */
static bool fetch(const char *url, int timeout_ms)
{
    s_short_wait = timeout_ms < TTS_HTTP_TIMEOUT_MS;
    bool ok = fetch_once(url, timeout_ms);
    s_short_wait = false;
    if (ok) {
        return true;
    }
    if (!s_payload_open && !s_abort && s_fetch_gen == s_gen) {
        ESP_LOGW(TAG, "nothing decoded (%u bytes), retrying once", (unsigned)s_bytes);
        vTaskDelay(pdMS_TO_TICKS(TTS_RETRY_DELAY_MS));
        if (fetch_once(url, TTS_HTTP_TIMEOUT_MS)) {
            return true;
        }
    }
    return false;
}


/* The endpoint template. Empty fields fall back to the build, which is how the
 * public tree ships (no service, so replies stay silent). A field the user set
 * on the settings page wins: the query is assembled here, so neither the shared
 * secret nor the voice has to be compiled in.
 *
 *   <url>[?|&]k=<key>&voice=<voice>&rate=<rate>&text={text}
 *
 * The {text} placeholder is what fetch_reply() splits on, so both paths share
 * one expansion. */
/* Into the caller's buffer: the reply worker and the reachability probe can ask
 * at the same time. */
static const char *tts_template_into(char *buf, size_t cap)
{
    char url[MUSE_SPEECH_URL_MAX + 1] = {0}, voice[MUSE_SPEECH_VOICE_MAX + 1] = {0};
    char key[MUSE_SPEECH_KEY_MAX + 1] = {0}, rate[MUSE_SPEECH_RATE_MAX + 1] = {0};
    muse_settings_speech(url, sizeof(url), voice, sizeof(voice), key, sizeof(key), rate, sizeof(rate));
    if (!url[0]) {
        strlcpy(buf, CONFIG_MUSE_TTS_URL, cap);
        return buf;
    }
    /* The settings page asks for a bare host, the way the Muse server row does,
     * so a value without a scheme gets https:// here. A full URL is used as
     * written. */
    if (strstr(url, "://")) {
        strlcpy(buf, url, cap);
    } else {
        strlcpy(buf, "https://", cap);
        strlcat(buf, url, cap);
    }
    bool have_query = strchr(buf, '?') != NULL;
    if (key[0]) {
        strlcat(buf, have_query ? "&k=" : "?k=", cap);
        strlcat(buf, key, cap);
        have_query = true;
    }
    if (voice[0]) {
        strlcat(buf, have_query ? "&voice=" : "?voice=", cap);
        strlcat(buf, voice, cap);
        have_query = true;
    }
    if (rate[0]) {
        strlcat(buf, have_query ? "&rate=" : "?rate=", cap);
        strlcat(buf, rate, cap);
        have_query = true;
    }
    strlcat(buf, have_query ? "&text={text}" : "?text={text}", cap);
    return buf;
}

static const char *tts_template(void)
{
    static char buf[TTS_TEMPLATE_CAP];
    return tts_template_into(buf, sizeof(buf));
}

/* Expand the template with one short text, for callers that fetch one
 * pre-generated word audio file instead of a reply sentence: the
 * bridge answers /tts with the cached MP3 when it has one, so this is
 * the same endpoint, host, key and voice the replies already use, and
 * no second server or second host setting is needed. The word audio
 * is pre-generated on the host (see ~/workspace/wordcards-tts-server.md);
 * nothing here synthesises.
 *
 * The request is marked word=1 (appended after the text, which is
 * always last in the template): the bridge then ignores the reply
 * voice/rate from the query and answers with the word voice
 * (en-US-AriaNeural, -10%) from its disk cache. A Chinese reply voice
 * must never read English words, and the flag is what keeps the two
 * apart on the one endpoint. */
bool muse_tts_url_for_text(const char *text, char *out, size_t cap)
{
    if (!text || !text[0] || !out || cap == 0) {
        return false;
    }
    char tpl[TTS_TEMPLATE_CAP];
    const char *t = tts_template_into(tpl, sizeof(tpl));
    const char *ph = strstr(t, "{text}");
    if (!t[0] || !ph) {
        return false;
    }
    size_t prefix_len = (size_t)(ph - t);
    const char *suffix = ph + 6;
    size_t text_len = strlen(text);
    char encoded[TTS_TEXT_CHUNK * 3 + 8];
    if (text_len > TTS_TEXT_CHUNK ||
        url_encode(encoded, sizeof(encoded), text, text_len) == 0) {
        return false;
    }
    /* The template always ends with the text placeholder, and the
     * encoded text carries no '&', so the flag lands as its own
     * query parameter. */
    static const char word_flag[] = "&word=1";
    if (prefix_len + strlen(encoded) + strlen(suffix) +
            sizeof(word_flag) > cap) {
        return false;
    }
    memcpy(out, t, prefix_len);
    strcpy(out + prefix_len, encoded);
    strcat(out + prefix_len, suffix);
    strcat(out + prefix_len, word_flag);
    return true;
}

static void fetch_reply(char *text)
{
    const char *url = tts_template();
    const char *ph = strstr(url, "{text}");
    size_t prefix_len = ph ? (size_t)(ph - url) : strlen(url);
    const char *suffix = ph ? ph + 6 : "";
    char encoded[TTS_TEXT_CHUNK * 3 + 8];

    size_t pos = 0;
    size_t total = strlen(text);
    int timeout_ms = s_reply_first ? TTS_HTTP_FIRST_MS : TTS_HTTP_TIMEOUT_MS;
    bool first_req = s_reply_first;
    bool last_unit = s_last_unit;
    s_reply_first = false;
    s_fetch_gen = s_gen;   /* from here on, bytes of an older generation stop here */
    while (pos < total && !s_abort && s_fetch_gen == s_gen) {
        size_t n = chunk_len(text + pos, total - pos);
        if (url_encode(encoded, sizeof(encoded), text + pos, n) == 0) {
            ESP_LOGW(TAG, "TTS text chunk does not fit the URL");
            break;
        }
        const size_t enc_len = strlen(encoded);
        const size_t suf_len = strlen(suffix);
        /* C1: the first request of a reply carries &first=1, so the bridge
         * cuts the first sentence at its first comma and streams the head
         * first; the tail follows in the same response.
         * C2: the last request of the message's final unit carries &last=1,
         * so the bridge can mark the last chunk of the reply. */
        const char *first_flag = (first_req && pos == 0) ? "&first=1" : "";
        const char *last_flag = (last_unit && pos + n >= total) ? "&last=1" : "";
        const size_t flag_len = strlen(first_flag) + strlen(last_flag);
        size_t need = prefix_len + enc_len + suf_len + flag_len + 1;
        char *u = (char *)malloc(need);
        if (!u) break;
        /* Assembled by hand: the template is a runtime string now (it can come
         * from the settings page), so snprintf would trip format-truncation. */
        memcpy(u, url, prefix_len);
        memcpy(u + prefix_len, encoded, enc_len);
        memcpy(u + prefix_len + enc_len, suffix, suf_len);
        memcpy(u + prefix_len + enc_len + suf_len, first_flag, strlen(first_flag));
        memcpy(u + prefix_len + enc_len + suf_len + strlen(first_flag),
               last_flag, strlen(last_flag) + 1);
        bool ok = fetch(u, timeout_ms);
        timeout_ms = TTS_HTTP_TIMEOUT_MS;
        free(u);
        if (!ok) break;
        pos += n;
    }

}

/* One long-lived worker: created once with its stack in PSRAM, then woken per
 * reply. It never exits, so there is no caps-aware self-delete to get wrong
 * (and vStreamBufferDeleteWithCaps() double-frees on IDF 6.0.1, esp-idf#18855). */
static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Drain whatever was queued: a barge-in leaves the old fetch in flight,
         * so the reply that replaces it waits here rather than go silent. */
        for (;;) {
            taskENTER_CRITICAL(&s_lock);
            char *text = s_pending;
            s_pending = NULL;
            taskEXIT_CRITICAL(&s_lock);
            if (!text) {
                break;
            }
            taskENTER_CRITICAL(&s_lock);
            bool dirty = s_ring_dirty;
            s_ring_dirty = false;
            taskEXIT_CRITICAL(&s_lock);
            if (dirty) {
                /* A cancelled barge-in fetch left bytes in the ring (speak's
                 * reset failed because this task was still blocked in
                 * ring_send). Only this task writes the ring and
                 * muse_tts_read() never blocks, so the reset here cannot
                 * fail: old bytes must never open the next reply. */
                xStreamBufferReset(s_ring);
            }
            fetch_reply(text);
            free(text);
        }
        taskENTER_CRITICAL(&s_lock);
        if (!s_pending) {   /* a reply queued meanwhile keeps the worker busy */
            s_busy = false;
            s_fetch_done = true;
        }
        taskEXIT_CRITICAL(&s_lock);
    }
}

/* The probe: open the connection (TCP + TLS + cert chain) and leave it in the
 * keep-alive pool, so the first sentence of the reply skips the handshake. The
 * endpoint answers 403 without the token; that is fine -- the socket is what
 * this buys, and the status is ignored. */
static void prewarm_task(void *arg)
{
    char *url = (char *)arg;
    int64_t t0 = esp_timer_get_time();
    s_prewarm = true;
    bool ok = ensure_client(url, TTS_PREWARM_TIMEOUT_MS);
    if (ok) {
        esp_err_t err = esp_http_client_perform(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "prewarm failed: %s", esp_err_to_name(err));
            drop_client();
        } else {
            ESP_LOGI(TAG, "prewarm: connection ready in %d ms",
                     (int)((esp_timer_get_time() - t0) / 1000));
        }
    }
    s_prewarm = false;
    free(url);
    s_prewarm_task = NULL;
    vTaskDelete(NULL);
}

void muse_tts_prewarm(void)
{
    if (s_task || s_prewarm_task) {
        return;   /* a reply is already fetching, or a probe is running */
    }
    if (s_client && s_client_last_us &&
        esp_timer_get_time() - s_client_last_us < TTS_IDLE_DROP_US) {
        return;   /* the socket from the last turn is still fresh */
    }
    if (s_client) {
        drop_client();   /* stale: reconnect now rather than on the first word */
    }
    char root[192];
    if (!tts_root_url(root, sizeof(root))) {
        return;
    }
    char *arg = strdup(root);
    if (!arg) {
        return;
    }
    if (xTaskCreateWithCaps(prewarm_task, "muse_tts_warm", TTS_PREWARM_STACK,
                            arg, TTS_TASK_PRIO, &s_prewarm_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        free(arg);
        s_prewarm_task = NULL;
    }
}

bool muse_tts_speak(const char *text, bool last)
{
    if (!tts_template()[0] || s_busy || !text || !text[0]) {
        return false;
    }
    if (!s_ring) {
        /* PSRAM: a 16 KB stream buffer out of internal RAM starves the rest. */
        s_ring = xStreamBufferCreateWithCaps(TTS_RING_BYTES, 1, MALLOC_CAP_SPIRAM);
        if (!s_ring) {
            ESP_LOGW(TAG, "no stream buffer for TTS audio");
            return false;
        }
    }
    if (!s_task) {
        /* 12 KB of stack does not fit internal RAM here (largest free block was
         * 9 KB on the 185B), so the worker's stack comes from PSRAM. */
        if (xTaskCreateWithCaps(tts_task, "muse_tts", TTS_TASK_STACK, NULL,
                                TTS_TASK_PRIO, &s_task,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            s_task = NULL;
            ESP_LOGW(TAG, "no task for TTS (PSRAM stack)");
            return false;
        }
    }
    /* A reset fails while this task's previous fetch still has the worker
     * blocked in ring_send; that failure is handed to the worker as
     * s_ring_dirty, which clears the ring before the new text is fetched. */
    bool ring_dirty = xStreamBufferReset(s_ring) != pdTRUE;
    s_reply_first = true;
    char *copy = strdup(text);
    if (!copy) return false;
    taskENTER_CRITICAL(&s_lock);
    char *stale = s_pending;   /* a cancelled fetch may have left one behind */
    s_pending = copy;
    s_abort = false;
    s_fetch_done = false;
    s_busy = true;
    s_gen++;
    s_last_unit = last;
    s_last_chunk = false;   /* no last chunk seen yet for this message */
    if (ring_dirty) s_ring_dirty = true;
    taskEXIT_CRITICAL(&s_lock);
    free(stale);
    xTaskNotifyGive(s_task);
    return true;
}

/* Queue one more request behind the one in flight: the next sentence's audio
 * arrives while this one is still being read out, so the gap between sentences
 * is only the service's first byte, not its whole synthesis time. The ring is
 * deliberately not reset - the bytes of the sentence playing are in it. */
bool muse_tts_enqueue(const char *text, bool last)
{
    if (!tts_template()[0] || s_busy || !s_ring || !s_task || !text || !text[0]) {
        return false;
    }
    char *copy = strdup(text);
    if (!copy) return false;
    s_reply_first = false;     /* queued behind audio already playing: no rush */
    taskENTER_CRITICAL(&s_lock);
    char *stale = s_pending;   /* the queue is one deep: this is a leftover */
    s_pending = copy;
    s_fetch_done = false;
    s_busy = true;
    /* Every queued sentence is its own request, so an abort raised while the
     * previous one was fetched must not outlive it. s_abort is set by a full
     * ring, a payload that is not MP3, or a stalled socket -- all per-request
     * failures. Only muse_tts_speak() used to clear it, which is once per
     * reply: after one failed sentence the streaming path queued the rest
     * with s_abort still true, fetch_reply() skipped its loop, and the whole
     * reply after the first sentence played silent. */
    s_abort = false;
    s_last_unit = last;
    taskEXIT_CRITICAL(&s_lock);
    free(stale);
    xTaskNotifyGive(s_task);
    return true;
}

size_t muse_tts_read(uint8_t *out, size_t cap)
{
    if (!s_ring) return 0;
    return xStreamBufferReceive(s_ring, out, cap, 0);
}

bool muse_tts_drained(void)
{
    return s_fetch_done && (!s_ring || xStreamBufferIsEmpty(s_ring));
}

/* C4: a new turn invalidates whatever the old turn was still fetching. The
 * fetch paths all check the generation on every chunk, so the bytes of a
 * fetch that started before this call are dropped, never played. */
void muse_tts_new_turn(void)
{
    s_gen++;
}

bool muse_tts_last_chunk(void)
{
    return s_last_chunk;
}

bool muse_tts_active(void)
{
    return s_busy;
}

void muse_tts_cancel(void)
{
    /* Barge-in. The fetch already on the wire cannot be stopped from here (the
     * IDF event handler's return value is ignored), but bumping the generation
     * makes it stop feeding the ring, and clearing s_busy lets the reply that
     * replaces it speak at once instead of coming out as a silent turn. */
    s_abort = true;
    s_gen++;
    s_last_chunk = false;
    s_last_unit = false;
    /* The reset fails while the worker is blocked in ring_send on the cancelled
     * fetch; s_ring_dirty makes the worker clear the ring before the next text
     * is fetched, so the old reply's tail cannot play into the new one. */
    bool ring_dirty = s_ring && xStreamBufferReset(s_ring) != pdTRUE;
    taskENTER_CRITICAL(&s_lock);
    s_busy = false;
    s_fetch_done = true;
    char *stale = s_pending;   /* drop queued text with the turn that owned it */
    s_pending = NULL;
    if (ring_dirty) s_ring_dirty = true;
    taskEXIT_CRITICAL(&s_lock);
    free(stale);
}

/* ---------- reachability probe ---------- */

/* What the settings page shows used to be inferred from the configuration: any
 * non-empty URL read "Connected", so a host that stopped resolving still looked
 * fine. This asks the service for a real reply instead - the same template, the
 * same key, one two-character line - so host, certificate, key and voice are
 * all exercised. Two characters is the cheapest request that proves speech
 * works, and it stays provider-independent: any service that answers MP3 works,
 * not just the one with a /health route. */
#define SPEECH_PROBE_TEXT "你好"
/* One TLS handshake on this stack; 8 KB was enough on paper and not in
 * practice, the same way TTS_TASK_STACK grew. */
#define SPEECH_PROBE_STACK 10240
#define SPEECH_PROBE_PRIO 3
#define SPEECH_PROBE_TIMEOUT_MS 6000
/* A body this small is an error page, not a line of speech. */
#define SPEECH_PROBE_MIN_BYTES 512

static portMUX_TYPE s_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_speech_state_t s_probe_state = MUSE_SPEECH_UNTESTED;
static char s_probe_detail[48];
/* Bumped whenever the settings change under a probe in flight, so a result that
 * belongs to the old endpoint is dropped instead of being reported as the
 * new one's. */
static volatile uint32_t s_probe_gen;
static volatile bool s_probe_running;
static volatile size_t s_probe_bytes;

static void probe_report(muse_speech_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_probe_lock);
    s_probe_state = state;
    strlcpy(s_probe_detail, detail, sizeof(s_probe_detail));
    portEXIT_CRITICAL(&s_probe_lock);
}

/* Is there an endpoint at all? Asked without touching tts_template()'s shared
 * static buffer: this runs on the UI task every tick, and that buffer belongs
 * to the reply worker. */
static bool speech_configured(void)
{
    char u[MUSE_SPEECH_URL_MAX + 1] = {0}, v[MUSE_SPEECH_VOICE_MAX + 1] = {0};
    char k[MUSE_SPEECH_KEY_MAX + 1] = {0}, r[MUSE_SPEECH_RATE_MAX + 1] = {0};
    muse_settings_speech(u, sizeof(u), v, sizeof(v), k, sizeof(k), r, sizeof(r));
    return u[0] || CONFIG_MUSE_TTS_URL[0];
}

/* The reply URL with the placeholder already expanded: exactly what
 * fetch_reply() would put on the wire, minus the reply's own text. */
static bool probe_url(char *out, size_t cap)
{
    char tpl[TTS_TEMPLATE_CAP];
    const char *t = tts_template_into(tpl, sizeof(tpl));
    const char *ph = strstr(t, "{text}");
    if (!t[0] || !ph) {
        return false;
    }
    char enc[sizeof(SPEECH_PROBE_TEXT) * 3];
    if (url_encode(enc, sizeof(enc), SPEECH_PROBE_TEXT, strlen(SPEECH_PROBE_TEXT)) == 0) {
        return false;
    }
    const size_t pre = (size_t)(ph - t), enc_len = strlen(enc), suf = strlen(ph + 6);
    if (pre + enc_len + suf + 1 > cap) {
        return false;
    }
    memcpy(out, t, pre);
    memcpy(out + pre, enc, enc_len);
    memcpy(out + pre + enc_len, ph + 6, suf + 1);
    return true;
}

static esp_err_t on_probe_data(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data_len > 0) {
        s_probe_bytes += (size_t)ev->data_len;
    }
    return ESP_OK;
}

/* One attempt. Content-Length is not trusted: the service streams its first
 * bytes, so the body is counted as it arrives. */
static bool probe_attempt(const char *url, const char *user_agent, int *status_out)
{
    s_probe_bytes = 0;
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = SPEECH_PROBE_TIMEOUT_MS,
        .buffer_size = TTS_HTTP_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = user_agent,
        .event_handler = on_probe_data,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        *status_out = 0;
        return false;
    }
    esp_err_t err = esp_http_client_perform(c);
    *status_out = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return err == ESP_OK && *status_out == 200 && s_probe_bytes >= SPEECH_PROBE_MIN_BYTES;
}

static void probe_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    char url[MUSE_SPEECH_URL_MAX + MUSE_SPEECH_KEY_MAX + MUSE_SPEECH_VOICE_MAX +
             MUSE_SPEECH_RATE_MAX + 64];
    char detail[48] = "No answer";
    muse_speech_state_t state = MUSE_SPEECH_UNREACHABLE;

    const esp_app_desc_t *app = esp_app_get_description();
    char user_agent[48];
    snprintf(user_agent, sizeof(user_agent), "MuseGadget/%s", app->version);

    if (!probe_url(url, sizeof(url))) {
        strlcpy(detail, "No {text} in the URL", sizeof(detail));
    } else {
        /* Twice: the first request after an idle spell pays the TLS handshake
         * and can miss the timeout with the service perfectly healthy. */
        for (int i = 0; i < 2; i++) {
            int status = 0;
            if (probe_attempt(url, user_agent, &status)) {
                state = MUSE_SPEECH_REACHABLE;
                detail[0] = '\0';
                break;
            }
            if (status == 401 || status == 403) {
                /* The gate answered: the host is fine, the key is not. Saying
                 * "Can't connect" here would send the user after the network. */
                strlcpy(detail, "Key rejected", sizeof(detail));
            } else if (status) {
                snprintf(detail, sizeof(detail), "HTTP %d", status);
            } else {
                strlcpy(detail, "No answer", sizeof(detail));
            }
            if (i == 0) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }

    if (gen == s_probe_gen) {
        probe_report(state, detail);
    }
    s_probe_running = false;
    vTaskDelete(NULL);
}

void muse_speech_test(void)
{
    if (!speech_configured() || !muse_wifi_connected() || s_probe_running) {
        return;
    }
    s_probe_running = true;
    probe_report(MUSE_SPEECH_TESTING, "Testing...");
    if (xTaskCreateWithCaps(probe_task, "muse_tts_probe", SPEECH_PROBE_STACK,
                            (void *)(uintptr_t)s_probe_gen, SPEECH_PROBE_PRIO, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_probe_running = false;
        probe_report(MUSE_SPEECH_UNREACHABLE, "No task for the probe");
    }
}

void muse_speech_status(muse_speech_status_t *out)
{
    out->detail[0] = '\0';
    if (!speech_configured()) {
        out->state = MUSE_SPEECH_NOT_SET;
        strlcpy(out->detail, "No speech service", sizeof(out->detail));
        return;
    }
    portENTER_CRITICAL(&s_probe_lock);
    out->state = s_probe_state;
    strlcpy(out->detail, s_probe_detail, sizeof(out->detail));
    portEXIT_CRITICAL(&s_probe_lock);
    if (!muse_wifi_connected() && out->state != MUSE_SPEECH_TESTING) {
        out->state = MUSE_SPEECH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (out->state == MUSE_SPEECH_UNTESTED && !out->detail[0]) {
        strlcpy(out->detail, "Tap Test connection", sizeof(out->detail));
    }
}

const char *muse_speech_state_name(muse_speech_state_t state)
{
    switch (state) {
    case MUSE_SPEECH_NOT_SET: return "Not set";
    case MUSE_SPEECH_OFFLINE: return "Offline";
    case MUSE_SPEECH_UNTESTED: return "Not tested";
    case MUSE_SPEECH_TESTING: return "Testing";
    case MUSE_SPEECH_REACHABLE: return "Connected";
    case MUSE_SPEECH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

void muse_speech_config_changed(void)
{
    s_probe_gen++;
    probe_report(MUSE_SPEECH_UNTESTED, "");
}

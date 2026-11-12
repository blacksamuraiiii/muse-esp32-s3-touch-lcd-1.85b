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
 * The production voice note path (muse_voice.c, unmodified) against
 * deterministic, hardware-free fakes: the press-qualification rules from
 * upstream d0a65ab (600 ms hold, 500 ms captured while held, pre-roll kept),
 * the waking-press behaviour, and sample order through the pre-roll ring.
 *
 * Adapted from upstream's muse_ptt_voice_harness.c at d0a65ab, minus the
 * parts this fork does not have: live calls (tap-to-call, END_CALL, the call
 * epoch), toggle recording, shutdown, and the 5 s hint (16adad8, not ported).
 * The audio levels differ on purpose: this fork keeps its own energy gate
 * (GATE_MARGIN_DB, MIN_PEAK_DBFS), so the idle pre-roll is quiet (a room)
 * and the note audio is loud (speech); upstream's harness uses a loud
 * pre-roll and near-silent notes, which our gate would rightly drop.
 *
 * Scenarios:
 *   50-57  note admission (note_held + record, direct calls)
 *   60-63  sample order through the ring, PSRAM and low-memory
 *   70-74, 78, 79, 82-84  waking presses and short holds through voice_task
 * Scenarios 58/59 (shutdown, call epoch) have no equivalent here and are
 * not run.
 */

/* Includes the entire, unmodified production translation unit. */
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "muse_voice.c"

struct test_queue {
    muse_input_event_t items[64];
    int n;
} queue;
static int scenario, frame, notes, writes;
static int64_t clock_us;
static int note_release_frame, note_read_fail_frame;
static size_t note_frames;
static size_t note_burst, note_max_burst;
static bool verify_pcm;
static int16_t *expected_pcm, *uploaded_pcm;
static size_t expected_frames, direct_blocks;
static size_t pending_chunks;
#if MUSE_LOW_MEM
static muse_adpcm_t expected_encoder;
#endif
static bool ready;
static muse_mode_t mode;
static int mode_sets;
static int64_t first_mode_us = -1;
static float level;
static char caption[400];
static jmp_buf finish;

/* Mirrors muse_input.c's post(): the event, stamped at post time. */
static void post(muse_ptt_t type, bool wake)
{
    muse_input_event_t ev = { .type = type, .wake = wake, .at_us = clock_us };
    xQueueSend(&queue, &ev, 0);
}

static void push(muse_ptt_t type)
{
    post(type, false);
}

BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t t)
{
    (void)t;
    struct test_queue *qq = q;
    assert(qq->n < 64);
    qq->items[qq->n++] = *(const muse_input_event_t *)p;
    return pdTRUE;
}
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *p, TickType_t t)
{
    (void)t;
    struct test_queue *qq = q;
    assert(qq->n < 64);
    memmove(qq->items + 1, qq->items, qq->n++ * sizeof(qq->items[0]));
    qq->items[0] = *(const muse_input_event_t *)p;
    return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t q, void *p, TickType_t t)
{
    (void)t;
    struct test_queue *qq = q;
    if (!qq->n) return 0;
    *(muse_input_event_t *)p = qq->items[0];
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *p, TickType_t t)
{
    if (!xQueuePeek(q, p, t)) return 0;
    struct test_queue *qq = q;
    --qq->n;
    memmove(qq->items, qq->items + 1, qq->n * sizeof(qq->items[0]));
    return pdTRUE;
}
int64_t esp_timer_get_time(void)
{
    return clock_us;
}
void vTaskDelay(TickType_t ms)
{
    clock_us += (int64_t)ms * 1000;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
                                           void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                           BaseType_t core, uint32_t caps)
{
    (void)fn;
    (void)name;
    (void)stack;
    (void)arg;
    (void)prio;
    (void)handle;
    (void)core;
    (void)caps;
    return pdPASS;   /* the harness drives voice_task() directly, never starts it */
}
void *heap_caps_malloc(size_t n, int caps)
{
    (void)caps;
    return malloc(n);
}
void *heap_caps_realloc(void *p, size_t n, int caps)
{
    (void)caps;
    return realloc(p, n);
}
size_t heap_caps_get_free_size(int caps)
{
    (void)caps;
    return 1u << 20;
}
size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return 1u << 20;
}
void muse_state_set_mode(muse_mode_t m)
{
    mode = m;
    ++mode_sets;
    if (first_mode_us < 0) first_mode_us = clock_us;
}
muse_mode_t muse_state_mode(float *s)
{
    if (s) *s = 0;
    return mode;
}
void muse_state_set_level(float v)
{
    level = v;
}
void muse_state_set_progress(float v)
{
    (void)v;
}
void muse_state_set_caption(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(caption, sizeof(caption), fmt, ap);
    va_end(ap);
}
void muse_state_poke(void)
{
}
void muse_state_nudge(void)
{
}
void muse_state_make_happy(void)
{
}
bool muse_state_asleep(void)
{
    return false;
}
bool muse_state_on_battery(void)
{
    return false;
}
void muse_state_wait_awake(uint32_t ms)
{
    vTaskDelay(ms);
}
int muse_settings_volume(void)
{
    return 20;
}
int muse_settings_mic_gain(void)
{
    return 30;
}
bool muse_wifi_connected(void)
{
    return ready;
}
void muse_wifi_apply(void)
{
}
void muse_wifi_power(muse_wifi_power_t p)
{
    (void)p;
}
bool muse_hatch_ready(void)
{
    return ready;
}
void muse_hatch_status(muse_hatch_status_t *s)
{
    s->state = ready ? MUSE_HATCH_REACHABLE : MUSE_HATCH_OFFLINE;
}
void muse_hatch_set_resting(bool v)
{
    (void)v;
}
esp_err_t muse_audio_init(int volume, int mic_gain_db)
{
    (void)volume;
    (void)mic_gain_db;
    return ESP_OK;
}
void muse_audio_power(bool v)
{
    (void)v;
}
void muse_audio_selftest(void)
{
}
void muse_audio_chirp(int v)
{
    (void)v;
}
void muse_audio_sfx(muse_sfx_t sfx)
{
    (void)sfx;
}
void muse_audio_loopback_test(int v)
{
    (void)v;
}
size_t muse_hatch_mp3_selftest(int16_t **p)
{
    *p = NULL;
    return 0;
}
/* The production RMS formula, so the fork's energy gate sees the same levels
 * the firmware would: quiet pre-roll is a room, loud notes are speech. */
float muse_audio_dbfs(const int16_t *p, size_t n)
{
    if (!n) return -100.0f;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)p[i] * p[i];
    float db = 10.0f * log10f((float)(acc / n / (32768.0 * 32768.0)) + 1e-10f);
    return db < -100.0f ? -100.0f : db;
}
float muse_audio_level(const int16_t *p, size_t n)
{
    assert(n);
    return (float)p[0] / 1000;
}

void muse_hatch_turn_begin(void)
{
    ++notes;
    if (scenario >= 70) longjmp(finish, 3);
}
void muse_hatch_turn_audio(const int16_t *p, size_t n)
{
    if (verify_pcm) {
        assert(note_frames + n <= MAX_FRAMES);
        memcpy(uploaded_pcm + note_frames, p, n * sizeof(*p));
    }
    note_frames += n;
    note_burst += n;
    if (note_burst > note_max_burst) note_max_burst = note_burst;
}
size_t muse_hatch_turn_audio_wait(const int16_t *p, size_t n, int ms)
{
    (void)ms;
    muse_hatch_turn_audio(p, n);
    return n;
}
void muse_hatch_turn_end(void)
{
}
void muse_hatch_turn_cancel(void)
{
}
muse_hatch_ev_t muse_hatch_turn_event(char *p, size_t n)
{
    (void)p;
    (void)n;
    return MUSE_HATCH_EV_NONE;
}
bool muse_hatch_turn_caption(size_t x, char *p, size_t n)
{
    (void)x;
    (void)p;
    (void)n;
    return false;
}
size_t muse_hatch_turn_read(int16_t *p, size_t n, int ms)
{
    (void)p;
    (void)n;
    (void)ms;
    return 0;
}

esp_err_t muse_audio_read(int16_t *p, size_t n)
{
    if (scenario >= 70) {
        /* Stop after the actual outer voice loop has handled the attempt,
         * before it can consume a newer DOWN or overwrite its result. */
        /* A waking tap leaves the screen as it was (IDLE): stop once it's handled. */
        int quiet_until = scenario == 78 ? 6 : scenario == 84 ? 21 : 1;
        if (frame > quiet_until && mode == MUSE_MODE_IDLE) longjmp(finish, 1);
        ++frame;
        clock_us += (scenario == 70 || scenario == 72 || scenario == 73) ? 40000 : 20000;
        assert(frame < 100);
        if ((scenario == 74 || scenario == 78) && frame == 5) post(MUSE_PTT_UP, false);
        if ((scenario == 82 || scenario == 84) && frame == 20) post(MUSE_PTT_UP, false);   /* 400 ms */
        if (scenario == 73 && frame == 18) post(MUSE_PTT_UP, false);
        for (size_t i = 0; i < n; i++) p[i] = 10000;
        return ((scenario == 70 || scenario == 72) && frame == 22) ||
            (scenario == 71 && frame == 10) ? ESP_FAIL : ESP_OK;
    }
    ++frame;
    clock_us += 20000;
    assert(frame < 1200);
    if (scenario >= 50) {
        note_burst = 0;
        if (frame == note_release_frame) push(MUSE_PTT_UP);
        for (size_t i = 0; i < n; i++) p[i] = scenario >= 60 ?
            (int16_t)((int)((i * 113 + frame * 97) % 50000) - 25000) : (int16_t)10000;
        if (verify_pcm && frame != note_read_fail_frame) {
            assert(expected_frames + n <= MAX_FRAMES);
#if MUSE_LOW_MEM
            if (pending_chunks) {
                /* Independently encode each backlogged input block, then
                 * compare the complete ordered upload against this reference. */
                uint8_t codes[MUSE_AUDIO_CHUNK / 2];
                muse_adpcm_t decoder = expected_encoder;
                muse_adpcm_encode_block(&expected_encoder, p, n, codes);
                muse_adpcm_decode_block(&decoder, codes, n, expected_pcm + expected_frames);
                pending_chunks = pending_chunks + 1 > PRE_CHUNKS ? pending_chunks + 1 - PRE_CHUNKS : 0;
            } else
#endif
            {
                memcpy(expected_pcm + expected_frames, p, n * sizeof(*p));
                direct_blocks++;
            }
            expected_frames += n;
        }
        return frame == note_read_fail_frame ? ESP_FAIL : ESP_OK;
    }
    for (size_t i = 0; i < n; i++) p[i] = (int16_t)(100 + frame);
    return ESP_OK;
}
esp_err_t muse_audio_write(const int16_t *p, size_t n)
{
    (void)p;
    (void)n;
    ++writes;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = atoi(argv[1]);
    s_queue = &queue;
    ready = true;
    s_pre = calloc(PRE_CAP_CHUNKS, sizeof(*s_pre));
    if (scenario >= 70) {
        clock_us = 1000000;
        if (scenario == 78 || scenario == 84) {
            /* Woken to re-read a reply, while Muse is out of reach. */
            mode = MUSE_MODE_IDLE;
            snprintf(caption, sizeof(caption), "PREVIOUS REPLY");
            ready = false;
        }
        post(MUSE_PTT_DOWN, scenario == 72 || scenario == 78 || scenario == 83 || scenario == 84);
        int outcome = setjmp(finish);
        if (!outcome) voice_task(NULL);
        if (scenario == 79 || scenario == 83) {
            assert(outcome == 3 && notes == 1 && clock_us >= 1600000);
            /* A waking press shows nothing until it qualifies as a note. */
            if (scenario == 83) assert(first_mode_us >= 1600000);
        } else {
            assert(!notes && !note_frames && !s_rec && !s_pre_fill && !level);
            assert(outcome == 1);
            if (scenario == 70 || scenario == 71 || scenario == 72)
                assert(!strcmp(caption, "MICROPHONE ERROR - TRY AGAIN"));
            if (scenario == 73 || scenario == 82 || (scenario == 74))
                assert(!strcmp(caption, "HOLD LONGER TO TALK"));
            /* A waking tap only wakes: no readiness check, mode or caption change. */
            if (scenario == 78 || scenario == 84)
                assert(!mode_sets && mode == MUSE_MODE_IDLE && !strcmp(caption, "PREVIOUS REPLY"));
            if (scenario == 70) assert(clock_us == 1880000 && !queue.n); /* no release: 880 ms hold */
        }
        drop_rec();
        free(s_pre);
        return 0;
    }
    if (scenario >= 50) {
        /* Run the actual note admission/recording path: no transport call is
         * allowed before both thresholds, even without a whole-note buffer. */
        clock_us = 1000000;
        for (int i = 0; i < PRE_CHUNKS; i++) {
            for (int j = 0; j < MUSE_AUDIO_CHUNK; j++) s_chunk[j] = 100;   /* quiet room */
            pre_put();
        }
        post(MUSE_PTT_DOWN, scenario == 53);
        assert(queue.items[0].at_us == clock_us);
        assert(got_event(MUSE_PTT_DOWN) && s_press_us == clock_us);
        note_release_frame = scenario == 50 ? 29 : scenario == 51 ? 30 : scenario == 54 ? 31 :
            scenario == 55 ? 25 : 36;
        if (scenario == 60) note_release_frame = 100;  /* steady direct PCM after draining */
        if (scenario == 61) note_release_frame = 30;   /* release at admission, with backlog */
        if (scenario == 62) note_release_frame = 1100; /* recording reaches MAX_FRAMES */
        if (scenario == 63) note_read_fail_frame = 32; /* final drain with backlog remaining */
        if (scenario == 52) {
            /* A release queued at 580 ms stays short when handled much later. */
            clock_us += 580000;
            post(MUSE_PTT_UP, false);
            clock_us += 1000000;
        }
        if (scenario == 54 || scenario == 55) clock_us += 600000; /* delayed voice task */
        if (scenario == 56) note_read_fail_frame = 10;
        note_result_t result = note_held(scenario == 53, scenario == 57);
        bool admitted = result == NOTE_ACCEPTED;
        bool expected = !(scenario == 50 || scenario == 52 || scenario == 55 || scenario == 56);
        assert(admitted == expected && !notes && !note_frames);
        if (scenario == 50 || scenario == 52) assert(result == NOTE_SHORT_HOLD);
        if (scenario == 55) assert(result == NOTE_INSUFFICIENT_CAPTURE);
        if (scenario == 56) assert(result == NOTE_CAPTURE_ERROR);
        if (admitted) {
            assert(clock_us - s_press_us >= 600000);
            assert(frame >= (scenario == 57 ? 35 : 25));
            if (scenario == 54) assert(frame == 25); /* 500 ms of new audio, despite idle pre-roll */
            if (scenario == 53 || scenario == 57) {
                for (size_t i = 0; i < s_pre_fill; i++) {
                    pre_get(i, s_chunk);
                    /* ADPCM's predictor can need a few samples to follow a
                     * sudden change from the synthetic stale idle signal. */
                    assert(s_chunk[MUSE_AUDIO_CHUNK - 1] > -1000);
                }
            }
            size_t prefix = s_pre_fill * MUSE_AUDIO_CHUNK;
            if (scenario >= 60) {
                expected_pcm = calloc(MAX_FRAMES, sizeof(*expected_pcm));
                uploaded_pcm = calloc(MAX_FRAMES, sizeof(*uploaded_pcm));
                assert(expected_pcm && uploaded_pcm);
                for (size_t i = 0; i < s_pre_fill; i++) {
                    pre_get(i, expected_pcm + expected_frames);
                    expected_frames += MUSE_AUDIO_CHUNK;
                }
#if MUSE_LOW_MEM
                pending_chunks = s_pre_fill > PRE_CHUNKS ? s_pre_fill - PRE_CHUNKS : 0;
                expected_encoder = s_pre_enc;
#endif
                verify_pcm = true;
            }
#if HOLD_NOTES
            s_rec = malloc(MAX_FRAMES * sizeof(*s_rec));
#endif
            char why[96];
            assert(record(why, sizeof(why)) && notes == 1);
            /* Prefix and capture-lag tail remain in the sent audio. Neither
             * was used to make a short press eligible. */
            if (scenario == 63) assert(note_frames == prefix + MUSE_AUDIO_CHUNK);
            else assert(note_frames >= prefix + TAIL_FRAMES);
            assert(frame <= note_release_frame + 7);
            if (verify_pcm) {
                assert(note_frames == expected_frames);
                assert(!memcmp(uploaded_pcm, expected_pcm, note_frames * sizeof(*uploaded_pcm)));
                if (scenario == 60 || scenario == 62) assert(direct_blocks > 0);
                if (scenario == 62) assert(note_frames == MAX_FRAMES);
                free(expected_pcm);
                free(uploaded_pcm);
            }
#if MUSE_LOW_MEM
            if (scenario != 63) assert(note_max_burst <= 2 * PRE_CHUNKS * MUSE_AUDIO_CHUNK);
#endif
            drop_rec();
        }
        free(s_pre);
        return 0;
    }
    (void)writes;
    free(s_pre);
    return 0;
}

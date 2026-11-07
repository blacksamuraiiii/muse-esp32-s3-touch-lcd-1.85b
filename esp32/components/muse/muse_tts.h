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
 * Speaking replies out loud.
 *
 * The SDK ships the playback half of this (the turn's MP3 buffer, minimp3 and
 * the resampler) but no speech service, so by default replies are shown at
 * reading pace and the speaker stays quiet. This fills that in: the reply text
 * goes to CONFIG_MUSE_TTS_URL and the MP3 that comes back is handed to the
 * turn, which decodes and plays it.
 *
 * The URL carries the text in a {text} placeholder, so any HTTP service that
 * answers with MP3 works without touching this file:
 *
 *   https://fanyi.baidu.com/gettts?lan=zh&text={text}&spd=5&source=web
 *   http://192.168.1.1:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}
 *
 * That second form is how an Edge TTS bridge slot in: the device side does not
 * care who synthesises, only that MP3 comes back.
 *
 * One fetch runs at a time, on its own task, feeding a stream buffer that the
 * chat session drains into the turn on its own task (the turn's MP3 buffer is
 * not shared). Long replies are split into several requests so the URL stays a
 * sane length and playback can start before the rest has been fetched.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Start speaking `text`. Returns false when a fetch is already running or the
// task could not be started. The text is copied, so the caller may free it.
bool muse_tts_speak(const char *text);

// Queue `text` behind the request in flight, so its audio starts arriving while
// the current one is still being read out. Meant for the streaming speaker's
// next sentence. The buffered audio is kept, unlike muse_tts_speak(), which
// starts a fresh reply. The queue is one deep: false while a request is in
// flight (muse_tts_active()) or when nothing has fetched yet.
bool muse_tts_enqueue(const char *text);

// Copy up to `cap` bytes of MP3 out of the fetch, without blocking. Returns how
// many were copied, 0 when nothing is ready.
size_t muse_tts_read(uint8_t *out, size_t cap);

// True once every request has finished and the buffer has been drained. Until
// then the fetch may still add more audio.
bool muse_tts_drained(void);

// True while a fetch is in flight (including while its audio is still buffered).
bool muse_tts_active(void);

// Abandon the fetch and drop whatever it buffered. Safe to call when idle.
void muse_tts_cancel(void);

/* Reachability probe, so the settings rows can tell the truth instead of
 * claiming "Connected" the moment a host is filled in. The probe asks the
 * service for one very short line through the same template the replies use,
 * so host, certificate, key and voice are all exercised; the state is what the
 * UI shows, and it is never inferred from the configuration alone. */
typedef enum {
    MUSE_SPEECH_NOT_SET = 0,   /* no endpoint in the settings or the build */
    MUSE_SPEECH_OFFLINE,       /* saved, but there is no Wi-Fi to try it on */
    MUSE_SPEECH_UNTESTED,      /* saved, not probed since it was last changed */
    MUSE_SPEECH_TESTING,       /* the probe's request is in flight */
    MUSE_SPEECH_REACHABLE,     /* it answered with audio */
    MUSE_SPEECH_UNREACHABLE,   /* it did not answer, or answered without audio */
} muse_speech_state_t;

typedef struct {
    muse_speech_state_t state;
    char detail[48];   /* one short line, e.g. "HTTP 403" or "Waiting for Wi-Fi" */
} muse_speech_status_t;

// Probe now. Does nothing without an endpoint, without Wi-Fi, or while a probe
// is already running.
void muse_speech_test(void);

// Snapshot of the last probe. No Wi-Fi overrides a stored result, the way
// muse_hatch_status() does, so a row cannot keep saying "Connected" on a
// network that went away.
void muse_speech_status(muse_speech_status_t *out);

const char *muse_speech_state_name(muse_speech_state_t state);

// The speech settings changed: whatever was probed no longer describes them.
void muse_speech_config_changed(void);

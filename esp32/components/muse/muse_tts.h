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

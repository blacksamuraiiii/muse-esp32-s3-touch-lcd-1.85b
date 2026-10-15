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

#pragma once

#include <stdbool.h>

/*
 * Play an audio file from a URL on the gadget's speaker.
 *
 * This is the audio counterpart of display.draw_url: Muse fetches the file and
 * hands us a URL, or an agent synthesises speech somewhere that does have a
 * TTS service and sends the result here.
 *
 * One at a time, on its own task. The speaker codec is opened for the
 * duration and left open afterwards - the voice task owns closing it (it opens
 * and closes around its own idle), so closing it here could pull the codec out
 * from under a turn that is already playing.
 *
 * MP3 only, which is what TTS services hand back; the SDK's minimp3 decoder
 * and a small resampler bring it to the codec's 16 kHz.
 */

// Result code/message are the strings the command layer reports: "ok", or
// something like "bad_status"/"unreachable" with a human-readable message.
typedef void (*muse_audio_play_done_cb_t)(bool ok, const char *code,
                                          const char *message, void *user);

// Start downloading and playing `url`. Returns false if something is already
// playing or the task could not be started. `url` is copied.
bool muse_audio_play_url(const char *url, muse_audio_play_done_cb_t done, void *user);

// Stop playback and drop the download. Safe to call when idle.
void muse_audio_play_stop(void);

// True while a download or its playback is running.
bool muse_audio_play_busy(void);

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

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The board's mic and speaker codecs share one duplex I2S bus, so both run at
 * the same format: 16 kHz, 16-bit, 2 slots. This module hides that and
 * exposes mono buffers. Captured audio is high-passed to remove DC and rumble.
 */

#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 320   /* 20 ms of mono frames */

esp_err_t muse_audio_init(int volume, int mic_gain_db);

/* Off closes the codecs: ADC, DAC, speaker amp and I2S stop. On reopens them
 * with the saved volume and mic gain. Reads and writes need it on. */
void muse_audio_power(bool on);

void muse_audio_set_volume(int volume);          /* 0..100 */
void muse_audio_set_mic_gain(int db);            /* 0..MUSE_MIC_GAIN_MAX */

/* Measures the real capture/playback rates and per-mic levels; logs the result. */
void muse_audio_selftest(void);

/* Plays tones while recording, across mic gains; logs levels. For tuning. */
void muse_audio_loopback_test(int volume);

/* Blocking read of `frames` mono samples (mic pair mixed down). */
esp_err_t muse_audio_read(int16_t *mono, size_t frames);

/* Blocking write of `frames` mono samples to the speaker. */
esp_err_t muse_audio_write(const int16_t *mono, size_t frames);

/* RMS of a buffer in dBFS (-100 for silence). */
float muse_audio_dbfs(const int16_t *mono, size_t frames);

/* RMS of a buffer mapped to a 0..1 perceptual level. */
float muse_audio_level(const int16_t *mono, size_t frames);

/* Short UI chirp: rising for "go", falling for "done". */
void muse_audio_chirp(int rising);

/* Game + daily sound effects (sfx-plan-v4): nine short sounds, all
 * synthesized on the fly from a note table with the muse_audio_chirp
 * recipe (16 kHz, sine + 0.3x 3rd harmonic, 10% attack then (1-x)^2
 * decay). No audio files, so retuning is a data change.
 * muse_audio_write already silences when the speaker is off and follows
 * the volume slider, so there is no separate game-volume setting. */
typedef enum {
    MUSE_SFX_GOOD,   /* score / coin / correct answer: 880 -> 1318 Hz, 120 ms */
    MUSE_SFX_BAD,    /* bomb / wrong answer / loss / fee refused: 300 -> 150 Hz, 120 ms */
    MUSE_SFX_DRAW,   /* draw / 0-coin settlement: 660 Hz, 120 ms */
    MUSE_SFX_PLACE,  /* tic-tac-toe placement / RPS punch: 900 Hz, 45 ms */
    MUSE_SFX_WIN,    /* won the round: 523 -> 659 -> 784 Hz arpeggio, 330 ms */
    MUSE_SFX_EAT,    /* feed paid: 740 Hz twice, 40 ms gap, 140 ms */
    MUSE_SFX_PET,    /* face tap: 523 Hz, 80 ms, half volume */
    MUSE_SFX_FORTUNE, /* fresh daily draw: 784 -> 988 -> 1174 Hz, 180 ms */
    MUSE_SFX_START,  /* start / play again: reuses muse_audio_chirp(1) */
} muse_sfx_t;

/* Longest table sound (WIN, 330 ms) in samples: the playback buffer size. */
#define MUSE_SFX_MAX_SAMPLES (MUSE_AUDIO_RATE * 330 / 1000)

/* Play one sound effect, blocking (the chirp idiom: in-play sounds are
 * <=120 ms, the 330 ms WIN only fires at OVER when nothing needs
 * responding). */
void muse_audio_sfx(muse_sfx_t sfx);

/* Pure synthesis: render `sfx` into `out` (capacity `cap` samples) and
 * return the samples written. No hardware, no state: the host test calls
 * this directly. MUSE_SFX_START is not a table sound -- muse_audio_sfx()
 * plays it via muse_audio_chirp(1) -- so it renders 0 samples. A render
 * into a short buffer is exactly the prefix of the full render (the
 * envelope and glide are computed against the true note length). */
static inline size_t muse_audio_sfx_render(muse_sfx_t sfx, int16_t *out, size_t cap)
{
    typedef struct {
        float f0, f1;   /* start / end frequency, Hz (f0 == f1: pure tone) */
        uint16_t ms;    /* note length */
        int16_t amp;    /* peak amplitude */
    } muse_sfx_note_t;
    typedef struct {
        uint8_t first;  /* first note of this sound in the table */
        uint8_t count;  /* notes in this sound */
    } muse_sfx_def_t;
    /* Table-driven: retune by changing numbers, not logic. GOOD, WIN
     * and FORTUNE are two/three notes back to back, each with its own
     * envelope and phase -- the same shape as the reference renders in
     * sfx-demo/gen-v4.py (one note() call per note, concatenated). EAT's
     * middle note is a 40 ms silent gap (amp 0), not a tone.
     * BAD is the loudest entry on purpose (user 2026-11-09: the wrong
     * answer / bomb sound was too quiet). Its old 200 -> 100 Hz sat in
     * the band where this small speaker gives almost no output, so the
     * glide was moved up to 300 -> 150 Hz and the peak raised 9000 ->
     * 18000 (peak 1.3 x amp = 23400, 71% of full scale, no clipping). */
    static const muse_sfx_note_t notes[] = {
        /* GOOD  */ {880.0f, 880.0f, 60, 7000}, {1318.0f, 1318.0f, 60, 7000},
        /* BAD   */ {300.0f, 150.0f, 120, 18000},
        /* DRAW  */ {660.0f, 660.0f, 120, 7000},
        /* PLACE */ {900.0f, 900.0f, 45, 5000},
        /* WIN   */ {523.0f, 523.0f, 110, 7000}, {659.0f, 659.0f, 110, 7000},
                    {784.0f, 784.0f, 110, 7000},
        /* EAT   */ {740.0f, 740.0f, 50, 7000}, {740.0f, 740.0f, 40, 0},
                    {740.0f, 740.0f, 50, 7000},
        /* PET   */ {523.0f, 523.0f, 80, 4000},
        /* FORTUNE */ {784.0f, 784.0f, 60, 7000}, {988.0f, 988.0f, 60, 7000},
                    {1174.0f, 1174.0f, 60, 7000},
    };
    static const muse_sfx_def_t defs[] = {
        [MUSE_SFX_GOOD] = {0, 2},
        [MUSE_SFX_BAD] = {2, 1},
        [MUSE_SFX_DRAW] = {3, 1},
        [MUSE_SFX_PLACE] = {4, 1},
        [MUSE_SFX_WIN] = {5, 3},
        [MUSE_SFX_EAT] = {8, 3},
        [MUSE_SFX_PET] = {11, 1},
        [MUSE_SFX_FORTUNE] = {12, 3},
    };
    if (sfx < 0 || sfx >= MUSE_SFX_START || out == NULL) {
        return 0;
    }
    /* 2*pi; M_PI is not defined under -std=c11 on the host. */
    const float two_pi = 6.2831855f;
    const muse_sfx_def_t def = defs[sfx];
    size_t written = 0;
    for (uint8_t k = 0; k < def.count; k++) {
        const muse_sfx_note_t *note = &notes[def.first + k];
        size_t full = (size_t)MUSE_AUDIO_RATE * note->ms / 1000;
        size_t n = full;
        if (written + n > cap) {
            n = cap - written;
        }
        float phase = 0.0f;
        for (size_t i = 0; i < n; i++) {
            /* x is against the true note length, so a short buffer
             * renders exactly the prefix of the full sound. */
            float x = (float)i / (float)full;
            float f = note->f0 + (note->f1 - note->f0) * x;
            phase += two_pi * f / (float)MUSE_AUDIO_RATE;
            float env = (x < 0.1f ? x / 0.1f : 1.0f) * (1.0f - x) * (1.0f - x);
            float s = sinf(phase) + 0.3f * sinf(3.0f * phase);
            out[written + i] = (int16_t)(s * env * (float)note->amp);
        }
        written += n;
        if (written >= cap) {
            break;
        }
    }
    return written;
}

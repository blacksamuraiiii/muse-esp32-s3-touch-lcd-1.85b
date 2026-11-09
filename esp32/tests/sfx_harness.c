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

/* Host harness for the game sound effects: muse_audio_sfx_render() is a
 * static inline in muse_audio.h, so it builds with plain cc on the host
 * (same pattern as test_speaking_face.py). Asserts each sound renders
 * non-silent audio at the expected length, with the envelope decaying to
 * ~0 at the end and no clipping, and that a short buffer renders exactly
 * the prefix of the full sound. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "muse_audio.h"

static int16_t s_buf[MUSE_SFX_MAX_SAMPLES];

static void check_sound(muse_sfx_t sfx, const char *name, size_t want_n)
{
    size_t n = muse_audio_sfx_render(sfx, s_buf, MUSE_SFX_MAX_SAMPLES);
    assert(n == want_n);
    int32_t peak = 0;
    int64_t sum = 0;
    int64_t sumsq = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = s_buf[i] < 0 ? -s_buf[i] : s_buf[i];
        if (v > peak) {
            peak = v;
        }
        sum += s_buf[i];
        sumsq += (int64_t)s_buf[i] * s_buf[i];
    }
    /* Non-silent, well under clipping, and the (1-x)^2 envelope decays
     * to ~0 by the last sample. */
    assert(peak > 1000);
    assert(peak < 20000);
    assert(sumsq > 0);
    assert(s_buf[n - 1] > -50 && s_buf[n - 1] < 50);
    printf("%s: n=%zu peak=%d sum=%lld sumsq=%lld\n", name, n, (int)peak,
           (long long)sum, (long long)sumsq);
}

int main(void)
{
    /* Lengths at 16 kHz: GOOD/BAD/DRAW 120 ms, PLACE 45 ms, WIN 330 ms,
     * EAT 140 ms, PET 80 ms, FORTUNE 180 ms. */
    check_sound(MUSE_SFX_GOOD, "good", 1920);
    check_sound(MUSE_SFX_BAD, "bad", 1920);
    check_sound(MUSE_SFX_DRAW, "draw", 1920);
    check_sound(MUSE_SFX_PLACE, "place", 720);
    check_sound(MUSE_SFX_WIN, "win", 5280);
    check_sound(MUSE_SFX_EAT, "eat", 2240);
    check_sound(MUSE_SFX_PET, "pet", 1280);
    check_sound(MUSE_SFX_FORTUNE, "fortune", 2880);
    /* EAT is two 50 ms peeps with a 40 ms silent gap in between:
     * samples 800..1439 are exactly 0, both peeps sound. */
    size_t n_eat = muse_audio_sfx_render(MUSE_SFX_EAT, s_buf, MUSE_SFX_MAX_SAMPLES);
    assert(n_eat == 2240);
    int32_t peep1 = 0, peep2 = 0;
    for (size_t i = 0; i < 800; i++) {
        int32_t v = s_buf[i] < 0 ? -s_buf[i] : s_buf[i];
        if (v > peep1) {
            peep1 = v;
        }
    }
    for (size_t i = 800; i < 1440; i++) {
        assert(s_buf[i] == 0);
    }
    for (size_t i = 1440; i < 2240; i++) {
        int32_t v = s_buf[i] < 0 ? -s_buf[i] : s_buf[i];
        if (v > peep2) {
            peep2 = v;
        }
    }
    assert(peep1 > 1000 && peep2 > 1000);
    /* START is not a table sound: it renders nothing (played via chirp). */
    assert(muse_audio_sfx_render(MUSE_SFX_START, s_buf, MUSE_SFX_MAX_SAMPLES) == 0);
    /* A short buffer renders exactly the prefix of the full sound. */
    static int16_t full[MUSE_SFX_MAX_SAMPLES];
    int16_t part[100];
    size_t nfull = muse_audio_sfx_render(MUSE_SFX_WIN, full, MUSE_SFX_MAX_SAMPLES);
    size_t npart = muse_audio_sfx_render(MUSE_SFX_WIN, part, 100);
    assert(nfull == 5280 && npart == 100);
    assert(memcmp(full, part, 100 * sizeof(int16_t)) == 0);
    /* Out of range / NULL renders nothing. */
    assert(muse_audio_sfx_render((muse_sfx_t)99, s_buf, MUSE_SFX_MAX_SAMPLES) == 0);
    assert(muse_audio_sfx_render(MUSE_SFX_GOOD, NULL, MUSE_SFX_MAX_SAMPLES) == 0);
    printf("ok\n");
    return 0;
}

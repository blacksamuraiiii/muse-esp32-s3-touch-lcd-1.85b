/* Host harness for the speaking-face port (upstream bd7ea66, face only).
 * Builds with plain cc on the host (same pattern as pet_economy_harness.c):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I components/muse tests/speaking_face_harness.c \
 *      avatar/muse_pixel.c -lm -o /tmp/speaking_face
 * Two parts:
 *  1. Engine: drive muse_speaking_update at 25 fps through scripted
 *     phrases and assert the face outputs actually move (mouth/gaze/nod),
 *     including the syllable-rhythm fallback for silent captions.
 *  2. Render: run the real muse_pixel_render through a speaking phrase
 *     and assert the mouth on screen opens and closes (dark pixels in the
 *     mouth box vary), so the wiring -- not just the engine -- is proven.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "muse_pixel.h"
#include "muse_speaking.h"

#define DT 0.04f /* frame_tick period on S3 boards (40 ms) */

static void step(muse_speaking_t *s, bool speaking, float mode_t, float level)
{
    muse_speaking_update(s, speaking, DT, mode_t, level);
}

/* --- 1. engine ------------------------------------------------------- */
static void test_engine_voiced(void)
{
    muse_speaking_t s = {0};
    float max_mouth = 0, max_gaze = 0, max_nod = 0;
    /* 5 s of speaking at level 0.8: voice is heard, mouth follows energy. */
    for (int i = 0; i < 125; i++) {
        step(&s, true, i * DT, 0.8f);
        if (s.mouth > max_mouth) max_mouth = s.mouth;
        if (fabsf(s.gaze) > max_gaze) max_gaze = fabsf(s.gaze);
        if (fabsf(s.nod) > max_nod) max_nod = fabsf(s.nod);
    }
    assert(max_mouth > 0.5f); /* energy 0.8 -> mouth ~1.0 */
    assert(max_gaze > 0.2f);  /* a raise drifts the gaze to one side */
    assert(max_nod > 0.1f);   /* steps/open nod the face */
    /* 1 s of silence afterwards: everything settles back to rest. */
    for (int i = 0; i < 25; i++)
        step(&s, false, 5.0f + i * DT, 0.0f);
    assert(s.mouth < 0.01f);
    assert(fabsf(s.gaze) < 0.01f);
    assert(fabsf(s.nod) < 0.01f);
}

static void test_engine_silent_caption(void)
{
    muse_speaking_t s = {0};
    float max_mouth = 0;
    int closed_frames = 0;
    /* 3 s of speaking with no audio at all: the syllable rhythm still
     * articulates, with a short phrase breath (mouth 0) each cycle. */
    for (int i = 0; i < 75; i++) {
        step(&s, true, i * DT, 0.0f);
        if (s.mouth > max_mouth) max_mouth = s.mouth;
        if (s.mouth == 0.0f) closed_frames++;
    }
    assert(!s.heard_voice);
    assert(max_mouth > 0.3f);   /* syllables open the mouth */
    assert(closed_frames > 0);  /* phrase breaths close it */
}

/* --- 2. render ------------------------------------------------------- */
#define N 64
static uint16_t s_buf[N * N];

/* Dark pixels in the mouth box (rows 39..43, cols 28..36): skin is light,
 * the mouth fill/outline is dark, and nothing else lives in that box. */
static int mouth_pixels(void)
{
    muse_pixel_set_size(N);
    muse_pixel_scale(s_buf, N, 0, N - 1, 0, N - 1);
    int n = 0;
    for (int y = 39; y <= 43; y++) {
        for (int x = 28; x <= 36; x++) {
            uint16_t c = s_buf[y * N + x];
            unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            if (r < 12 && g < 20 && b < 12)
                n++;
        }
    }
    return n;
}

static void test_render_speaking(void)
{
    int min_m = 1000, max_m = 0;
    /* 3 s of speaking at level 0.8, rendered through the real avatar. */
    for (int i = 0; i < 75; i++) {
        muse_pose_t p = { .mode = MUSE_MODE_SPEAKING, .t = 100.0f + i * DT,
                          .mode_t = i * DT, .level = 0.8f, .happy = 0.0f };
        muse_pixel_render(&p);
        int m = mouth_pixels();
        if (m < min_m) min_m = m;
        if (m > max_m) max_m = m;
    }
    /* The mouth opens wide at phrase peaks and closes between them. */
    assert(max_m > 10);
    assert(max_m > min_m);
}

int main(void)
{
    test_engine_voiced();
    test_engine_silent_caption();
    test_render_speaking();
    printf("ok\n");
    return 0;
}

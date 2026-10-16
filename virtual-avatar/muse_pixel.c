/*
 * Custom avatar: MaoMao (energetic Buttercup-style).
 *
 * 64x64 procedural pixel-art renderer for the muse-gadget-sdk avatar API.
 * Draws an energetic girl with short black hair, huge green eyes and a
 * big smile, with per-state animation (boot/idle/listening/thinking/
 * speaking/error/off + pet "happy" reaction).
 *
 * Drop this file at:  components/muse/avatar/muse_pixel.c
 * (that directory is gitignored; delete the file to go back to default)
 */

#include "muse_pixel.h"

#include <math.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

enum {
    C_BG = 0,
    C_OUT,      /* outline: near-black */
    C_SKIN,     /* peach */
    C_SKIND,    /* skin shadow */
    C_HAIR,     /* black hair */
    C_EYE,      /* eye white */
    C_IRIS,     /* green iris */
    C_IRISD,    /* iris shade */
    C_PUPIL,    /* black pupil */
    C_SHINE,    /* white highlight */
    C_MOUTH,    /* mouth dark */
    C_TONGUE,   /* tongue */
    C_DRESS,    /* green dress */
    C_STRIPE,   /* dress black stripe */
    C_BLUSH,    /* pink blush */
    C_GLOW,     /* state-tinted glow */
    C_COUNT,
};

static const uint32_t PAL_RGB[C_COUNT] = {
    [C_BG]     = 0x000000,
    [C_OUT]    = 0x201812,
    [C_SKIN]   = 0xffd9b3,
    [C_SKIND]  = 0xe8b48c,
    [C_HAIR]   = 0x181818,
    [C_EYE]    = 0xffffff,
    [C_IRIS]   = 0x58b368,
    [C_IRISD]  = 0x3a8a48,
    [C_PUPIL]  = 0x101010,
    [C_SHINE]  = 0xffffff,
    [C_MOUTH]  = 0x5c2420,
    [C_TONGUE] = 0xe07070,
    [C_DRESS]  = 0x58b368,
    [C_STRIPE] = 0x181818,
    [C_BLUSH]  = 0xf09890,
    [C_GLOW]   = 0x58b368,
};

/* State accent colours (also used for the glow). */
static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = 0x9fd0ff,
    [MUSE_MODE_IDLE]      = 0x58b368,
    [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING]  = 0xe07bff,
    [MUSE_MODE_SPEAKING]  = 0x6ff0bf,
    [MUSE_MODE_ERROR]     = 0xff5c5c,
    [MUSE_MODE_OFF]       = 0x555566,
};

static uint8_t  s_fb[W * H];
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint32_t s_accent = 0x58b368;
static int      s_size = 0;
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 19) & 0x1f) << 11 |
                      ((rgb >> 10) & 0x3f) << 5 |
                      ((rgb >> 3) & 0x1f));
}

static uint16_t dim565(uint16_t c)
{
    /* ~55% brightness for grid lines / off state. */
    uint16_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    r = (r * 14) >> 4; g = (g * 14) >> 4; b = (b * 14) >> 4;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void init_pal(void)
{
    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = rgb565(PAL_RGB[i]);
        s_pal_dim[i] = dim565(s_pal[i]);
    }
    /* Glow follows the live accent. */
    s_pal[C_GLOW] = rgb565(s_accent);
    s_pal_dim[C_GLOW] = dim565(s_pal[C_GLOW]);
}

/* ------------------------------------------------------------------ */
/* Draw primitives (framebuffer is palette indices)                    */
/* ------------------------------------------------------------------ */

static void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H)
        s_fb[y * W + x] = c;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint8_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            s_fb[y * W + x] = c;
}

static void fill_ellipse(int cx, int cy, int rx, int ry, uint8_t c)
{
    if (rx <= 0 || ry <= 0) return;
    for (int y = cy - ry; y <= cy + ry; y++) {
        for (int x = cx - rx; x <= cx + rx; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                px(x, y, c);
        }
    }
}

/* Outlined ellipse: fill then 1px outline. */
static void ellipse_o(int cx, int cy, int rx, int ry, uint8_t fill, uint8_t out)
{
    fill_ellipse(cx, cy, rx + 1, ry + 1, out);
    fill_ellipse(cx, cy, rx, ry, fill);
}

/* Down-pointing triangle (for bangs): apex at (cx, y_apex). */
static void tri_down(int cx, int y_top, int half_w, int h, uint8_t c)
{
    for (int i = 0; i < h; i++) {
        int w = half_w * (h - i) / h;
        for (int x = cx - w; x <= cx + w; x++)
            px(x, y_top + i, c);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return ACCENT[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, (size_t)n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++)
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

/* ------------------------------------------------------------------ */
/* MaoMao                                                              */
/* ------------------------------------------------------------------ */

/* Blink state. */
static float s_blink_t = 0;   /* time left with eyes closed */
static float s_next_blink = 2.0f;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Draw one huge eye. open: 1 = fully open, 0 = closed line. */
static void draw_eye(int cx, int cy, float open, int look_dx, int look_dy)
{
    if (open < 0.15f) {
        /* Closed: a curved lash line. */
        for (int x = -6; x <= 6; x++)
            px(cx + x, cy + (x * x) / 24, C_OUT);
        return;
    }
    int rx = 7, ry = (int)(9 * open + 0.5f);
    /* Sclera with outline. */
    ellipse_o(cx, cy, rx, ry, C_EYE, C_OUT);
    /* Iris (green), pupil, highlight — shift with look direction. */
    int ix = cx + look_dx, iy = cy + look_dy;
    int irx = (int)(rx * 0.72f), iry = (int)(ry * 0.78f);
    if (open > 0.5f) {
        fill_ellipse(ix, iy, irx, iry, C_IRIS);
        /* iris shading: darker crescent at bottom */
        fill_ellipse(ix, iy + iry / 2, irx, iry / 2, C_IRISD);
        fill_ellipse(ix, iy, irx - 1, iry - 2, C_IRIS);
        int prx = irx * 2 / 3, pry = iry * 2 / 3;
        fill_ellipse(ix, iy, prx, pry, C_PUPIL);
        /* The signature highlight: small white dot, upper-left. */
        fill_ellipse(ix - prx / 2, iy - pry / 2, 1, 1, C_SHINE);
        if (prx >= 3)
            px(ix - prx / 2 + 1, iy - pry / 2 + 1, C_SHINE);
    } else {
        /* Half-open: just iris + pupil slit. */
        fill_ellipse(ix, iy, irx, iry, C_IRIS);
        fill_ellipse(ix, iy, irx * 2 / 3, iry / 2, C_PUPIL);
    }
}

/* Mouth variants. */
static void draw_mouth(int cx, int cy, muse_mode_t mode, float level,
                       float happy, float t)
{
    switch (mode) {
    case MUSE_MODE_SPEAKING: {
        /* Opens with the audio level. */
        int h = 2 + (int)(level * 7);
        int w = 4 + (int)(level * 3);
        ellipse_o(cx, cy, w, h, C_MOUTH, C_OUT);
        if (h > 4)
            fill_ellipse(cx, cy + h / 3, w - 2, h / 3, C_TONGUE);
        break;
    }
    case MUSE_MODE_LISTENING:
        /* Small attentive "o". */
        ellipse_o(cx, cy, 2, 3, C_MOUTH, C_OUT);
        break;
    case MUSE_MODE_THINKING:
        /* Flat, slightly tilted. */
        for (int x = -4; x <= 4; x++)
            px(cx + x, cy + x / 4, C_OUT);
        break;
    case MUSE_MODE_ERROR:
        /* Worried wobble. */
        for (int x = -5; x <= 5; x++) {
            int y = cy - (x * x) / 12 + (int)(sinf(t * 9 + x) * 1.0f);
            px(cx + x, y, C_OUT);
        }
        break;
    case MUSE_MODE_OFF:
        /* Soft closed smile. */
        for (int x = -4; x <= 4; x++)
            px(cx + x, cy + (x * x) / 14, C_OUT);
        break;
    default: {
        /* Happy open smile; bigger when petted. */
        int w = 5 + (int)(happy * 3);
        int h = 4 + (int)(happy * 4);
        /* Smile arc. */
        for (int x = -w; x <= w; x++) {
            int y = cy - 2 + (x * x * h) / (w * w * 2);
            px(cx + x, y, C_OUT);
            if (happy > 0.3f)
                px(cx + x, y + 1, C_OUT);
        }
        if (happy > 0.3f) {
            fill_ellipse(cx, cy + 1, w - 2, h - 2, C_MOUTH);
            fill_ellipse(cx, cy + h / 2, w - 3, 2, C_TONGUE);
        }
        break;
    }
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_init;
    if (!s_init) { init_pal(); s_init = true; }
    s_accent = muse_pixel_accent(p->mode);
    s_pal[C_GLOW] = rgb565(s_accent);
    s_pal_dim[C_GLOW] = dim565(s_pal[C_GLOW]);

    muse_mode_t mode = p->mode;
    float t = p->t;
    float happy = clampf(p->happy, 0, 1);
    float level = clampf(p->level, 0, 1);

    /* Boot: fade the whole figure in over ~1s. */
    float fade = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.0f, 0, 1) : 1.0f;
    /* Off: dim everything. */
    float brightness = mode == MUSE_MODE_OFF ? 0.45f : 1.0f;
    (void)brightness; /* applied via palette choice below */

    /* Blink timing (not while off / error). */
    float eye_open = 1.0f;
    if (mode != MUSE_MODE_OFF && mode != MUSE_MODE_ERROR) {
        s_next_blink -= 0.04f;
        if (s_next_blink <= 0) { s_blink_t = 0.12f; s_next_blink = 2.2f + (t - (int)t); }
        if (s_blink_t > 0) { s_blink_t -= 0.04f; eye_open = 0.0f; }
    } else if (mode == MUSE_MODE_OFF) {
        eye_open = 0.0f;
    } else if (mode == MUSE_MODE_ERROR) {
        eye_open = 0.7f;
    }

    /* Gentle idle bob (frozen when off). */
    int bob = mode == MUSE_MODE_OFF ? 0 : (int)(sinf(t * 2.2f) * 1.2f);
    /* Listening lean-in. */
    int lean = mode == MUSE_MODE_LISTENING ? -1 : 0;

    /* Thinking: pupils glance side to side. */
    int look_dx = 0, look_dy = 0;
    if (mode == MUSE_MODE_THINKING) {
        look_dx = (int)(sinf(t * 3.1f) * 2.5f);
        look_dy = (int)(cosf(t * 2.3f) * 1.0f);
    } else if (mode == MUSE_MODE_LISTENING) {
        look_dy = -1;
    }

    memset(s_fb, C_BG, sizeof(s_fb));
    if (fade <= 0.01f)
        return;

    int cx = 32;
    int cy = 26 + bob + lean;

    /* -- Dress (bottom of frame) -- */
    fill_rect(20, 52 + bob / 2, 44, 63, C_DRESS);
    fill_rect(20, 56 + bob / 2, 44, 59 + bob / 2, C_STRIPE);
    /* shoulders */
    fill_ellipse(cx, 52 + bob / 2, 13, 5, C_DRESS);

    /* -- Head -- */
    ellipse_o(cx, cy, 20, 19, C_SKIN, C_OUT);
    /* soft shading bottom-right */
    fill_ellipse(cx + 7, cy + 8, 10, 8, C_SKIND);
    fill_ellipse(cx + 4, cy + 5, 11, 9, C_SKIN);

    /* -- Hair: back mass -- */
    fill_ellipse(cx, cy - 9, 22, 15, C_HAIR);
    /* redraw face over the hair's lower part */
    fill_ellipse(cx, cy + 2, 18, 16, C_SKIN);
    fill_ellipse(cx + 6, cy + 9, 9, 7, C_SKIND);
    fill_ellipse(cx + 3, cy + 6, 10, 8, C_SKIN);

    /* Bangs: Buttercup's V tufts on the forehead. */
    tri_down(cx - 11, cy - 16, 5, 9, C_HAIR);
    tri_down(cx - 3, cy - 18, 5, 11, C_HAIR);
    tri_down(cx + 6, cy - 18, 5, 11, C_HAIR);
    tri_down(cx + 13, cy - 15, 4, 8, C_HAIR);
    /* Side flips. */
    tri_down(cx - 21, cy - 8, 4, 10, C_HAIR);
    tri_down(cx + 21, cy - 8, 4, 10, C_HAIR);
    /* Cowlick. */
    for (int i = 0; i < 6; i++) {
        px(cx + 8 + i, cy - 24 - i / 2, C_HAIR);
        px(cx + 8 + i, cy - 23 - i / 2, C_HAIR);
    }

    /* -- Eyes (the huge signature ones) -- */
    int ey = cy + 2;
    int ex = mode == MUSE_MODE_LISTENING ? 8 : 7;
    draw_eye(cx - 10, ey, eye_open, look_dx, look_dy);
    draw_eye(cx + 10, ey, eye_open, look_dx, look_dy);
    (void)ex;

    /* -- Blush (stronger when happy) -- */
    uint8_t blush_c = happy > 0.25f ? C_BLUSH : C_BLUSH;
    fill_ellipse(cx - 15, ey + 7, 3, 2, blush_c);
    fill_ellipse(cx + 15, ey + 7, 3, 2, blush_c);
    if (happy > 0.5f) {
        fill_ellipse(cx - 15, ey + 7, 4, 2, blush_c);
        fill_ellipse(cx + 15, ey + 7, 4, 2, blush_c);
    }

    /* -- Mouth -- */
    draw_mouth(cx, cy + 14, mode, level, happy, t);

    /* -- State glow ring (subtle) -- */
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING ||
        mode == MUSE_MODE_SPEAKING) {
        int gr = 24 + (int)(sinf(t * 4.0f) * 1.5f);
        for (int a = 0; a < 64; a++) {
            float ang = a * TAU / 64;
            px(cx + (int)(cosf(ang) * gr), cy + (int)(sinf(ang) * gr), C_GLOW);
        }
    }
    /* Happy: little hearts. */
    if (happy > 0.4f) {
        int hx = cx - 24, hy = (int)(cy - 10 - (t * 8)) % 20;
        if (hy < 0) hy += 20;
        px(hx, cy - hy, C_BLUSH); px(hx + 1, cy - hy, C_BLUSH);
        px(hx, cy - hy - 1, C_BLUSH); px(hx + 1, cy - hy - 1, C_BLUSH);
    }

    /* Boot fade: punch BG holes proportional to (1-fade). */
    if (fade < 1.0f) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int h = (x * 31 + y * 17) & 0xff;
                if (h > (int)(fade * 255))
                    s_fb[y * W + x] = C_BG;
            }
    }
    /* Off: dim by remapping through the dim palette is done in scale;
       here just darken hair/skin slightly via overlay. */
    if (mode == MUSE_MODE_OFF) {
        for (int i = 0; i < W * H; i++)
            if (s_fb[i] != C_BG && ((i * 7) & 3) == 0)
                s_fb[i] = C_BG;
    }
}

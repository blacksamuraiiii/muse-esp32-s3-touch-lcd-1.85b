/*
 * Custom avatar: MaoMao (Buttercup-style), v8.
 *
 * 64x64 procedural pixel-art renderer for the muse-gadget-sdk avatar API.
 * Redesigned for the 360x360 round LCD (ESP32-S3-Touch-LCD-1.85B): the
 * character fills the frame, huge green eyes, a confident smirk, and the
 * green dress with its black stripe.
 *
 * The hair is one mass: no outline stroke anywhere (C_HAIRR is gone), so the
 * dome, both flips and the cheeks' locks merge into the background they sit on
 * and only the edge against skin is visible - the reference reads the same way.
 * Nothing sits above the eyes either: no brow, no lash stroke, just the eye's
 * own 1px ring (MaoMao has no brows; the band above the eye in the reference is
 * the fringe).
 *
 * v8 changes one thing only - the hair. It is no longer built from ellipses,
 * triangles and a per-row fringe loop, but is a 64x64 dot matrix traced off
 * the reference photo (virtual-avatar/maomao/毛毛头像.png). The photo's hair
 * is already flat black shapes on a light field, so it survives the downsample
 * to one cell per 22.7 px with no interpretation: the dome, the tall bare-skin
 * V up the forehead, the outward flip and the long crescent lock that hangs
 * past the eye are all read off the photo, not re-drawn. Face, eyes, brows,
 * blush, mouth, dress and the per-state animation are v7's, untouched.
 *
 * Per-state animation: boot fade, idle bob + blink, listening lean-in,
 * thinking glance, speaking mouth, error wobble, off dim, pet "happy"
 * reaction (big grin + floating heart).
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
    C_SKIN,     /* warm peach (won't blow out white on the LCD) */
    C_SKIND,    /* skin shadow */
    C_HAIR,     /* black hair */
    C_EYE,      /* eye white */
    C_IRIS,     /* green iris */
    C_IRISD,    /* iris shade */
    C_PUPIL,    /* black pupil */
    C_SHINE,    /* white highlight */
    C_MOUTH,    /* mouth dark */
    C_TONGUE,   /* tongue */
    C_TEETH,    /* teeth white */
    C_DRESS,    /* green dress */
    C_STRIPE,   /* dress black stripe */
    C_BLUSH,    /* pink blush */
    C_GLOW,     /* state-tinted glow */
    C_COUNT,
};

static const uint32_t PAL_RGB[C_COUNT] = {
    [C_BG]     = 0x000000,
    [C_OUT]    = 0x1a1410,
    [C_SKIN]   = 0xffc890,
    [C_SKIND]  = 0xe0a068,
    [C_HAIR]   = 0x141414,
    [C_EYE]    = 0xffffff,
    [C_IRIS]   = 0x3fae5a,
    [C_IRISD]  = 0x2b7a40,
    [C_PUPIL]  = 0x0c0c0c,
    [C_SHINE]  = 0xffffff,
    [C_MOUTH]  = 0x5c2420,
    [C_TONGUE] = 0xe07070,
    [C_TEETH]  = 0xffffff,
    [C_DRESS]  = 0x3fae5a,
    [C_STRIPE] = 0x141414,
    [C_BLUSH]  = 0xf09890,
    [C_GLOW]   = 0x3fae5a,
};

/* State accent colours (also used for the glow). */
static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = 0x9fd0ff,
    [MUSE_MODE_IDLE]      = 0x3fae5a,
    [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING]  = 0xe07bff,
    [MUSE_MODE_SPEAKING]  = 0x6ff0bf,
    [MUSE_MODE_ERROR]     = 0xff5c5c,
    [MUSE_MODE_OFF]       = 0x555566,
};

static uint8_t  s_fb[W * H];
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint32_t s_accent = 0x3fae5a;
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

/*
 * v8 hair: a dot matrix, one bit per cell, traced off the reference photo.
 *
 * Row r of the table is canvas row r when the head sits on its nominal line
 * (cy == 28), so the whole block simply rides the idle bob by HAIR_ROW0's
 * offset. Bit (63 - x) belongs to column x; reading the table left to right
 * is reading the head left to right.
 *
 * The matrix was produced by virtual-avatar/tools/hair_trace.py: the photo's
 * black hair -> the eye/mouth/jaw linework removed -> an opening wide enough
 * to erase the bold outlines -> area average per cell -> 0.45 threshold ->
 * mirror the left half onto the right (the photo is drawn slightly rotated,
 * the two locks differ by up to 3 cells).
 */
#define HAIR_ROW0 28
static const uint64_t HAIR_BITS[64] = {
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000ff8000000ULL, 0x000000ffff800000ULL, 0x000003ffffe00000ULL,
    0x00000ffffff80000ULL, 0x00003ffffffe0000ULL, 0x00007fff7fff0000ULL, 0x0000fffe3fff8000ULL,
    0x0001fffc1fffc000ULL, 0x0003fffc1fffe000ULL, 0x0007fff80ffff000ULL, 0x0007fff007fff000ULL,
    0x000ffff007fff800ULL, 0x040f80000000f810ULL, 0x071e000000003c70ULL, 0x07f8000000000ff0ULL,
    0x07f00000000007f0ULL, 0x07f00000000007f0ULL, 0x07e00000000003f0ULL, 0x07e00000000003f0ULL,
    0x03e00000000003e0ULL, 0x03c00000000001e0ULL, 0x03c00000000001e0ULL, 0x03c00000000001e0ULL,
    0x01c00000000001c0ULL, 0x01c00000000001c0ULL, 0x01c00000000001c0ULL, 0x00c0000000000180ULL,
    0x00c0000000000180ULL, 0x0060000000000300ULL, 0x0060000000000300ULL, 0x0020000000000200ULL,
    0x0030000000000600ULL, 0x0030000000000600ULL, 0x0018000000000c00ULL, 0x0018000000000c00ULL,
    0x000c000000001800ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
};

/*
 * Half-width of the bare-skin V up the forehead, for rows cy-18 .. cy-12.
 * The photo's hairline rises in a V nearly to the crown, well above the face
 * ellipse's top row (cy-16), so those first rows would punch background holes
 * through the crown if the V were left as a hole in the matrix. Painting the
 * V in skin instead keeps the peak and hides the face outline underneath it.
 */
static const uint8_t WEDGE_HALF[7] = { 0, 1, 2, 2, 3, 4, 4 };

static void draw_hair(int cy)
{
    int off = cy - HAIR_ROW0;
    for (int r = 0; r < H; r++) {
        uint64_t bits = HAIR_BITS[r];
        if (!bits)
            continue;
        for (int x = 0; x < W; x++)
            if (bits & (1ULL << (63 - x)))
                px(x, r + off, C_HAIR);
    }
    /* The forehead V: skin, not hair, and no outline on either side. */
    for (int i = 0; i < 7; i++) {
        int y = cy - 18 + i;
        for (int dx = -WEDGE_HALF[i]; dx <= WEDGE_HALF[i]; dx++)
            px(32 + dx, y, C_SKIN);
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
/* MaoMao v2                                                           */
/* ------------------------------------------------------------------ */

/* Blink state. */
static float s_blink_t = 0;   /* time left with eyes closed */
static float s_next_blink = 2.0f;
static float s_heart_y = 18.0f;   /* the happy heart's row, rising as it floats */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Draw one huge eye. open: 1 = fully open, 0 = closed line. */
static void draw_eye(int cx, int cy, float open, int look_dx, int look_dy)
{
    if (open < 0.15f) {
        /* Closed: a curved lash line. */
        for (int x = -8; x <= 8; x++)
            px(cx + x, cy + (x * x) / 30, C_OUT);
        return;
    }
    int rx = 8, ry = (int)(10 * open + 0.5f);
    /* Sclera with outline, and nothing else above it: v8 drops the lash stroke
     * that used to ride one row above the ellipse. At three screen pixels a cell
     * that stroke was indistinguishable from a brow, and MaoMao has none - in
     * the reference the band over the eye is the fringe, not a line. */
    ellipse_o(cx, cy, rx, ry, C_EYE, C_OUT);
    /* Iris (green), pupil, highlights — shift with look direction. */
    int ix = cx + look_dx, iy = cy + look_dy;
    int irx = (int)(rx * 0.74f), iry = (int)(ry * 0.8f);
    if (open > 0.5f) {
        fill_ellipse(ix, iy, irx, iry, C_IRIS);
        /* iris shading: darker crescent at bottom */
        fill_ellipse(ix, iy + iry / 2, irx, iry / 2, C_IRISD);
        fill_ellipse(ix, iy, irx - 1, iry - 2, C_IRIS);
        int prx = irx * 2 / 3, pry = iry * 2 / 3;
        fill_ellipse(ix, iy, prx, pry, C_PUPIL);
        /* Big signature highlight upper-left + tiny sparkle lower-right. */
        fill_ellipse(ix - prx / 2 - 1, iy - pry / 2 - 1, 2, 2, C_SHINE);
        px(ix + prx / 2, iy + pry / 2, C_SHINE);
    } else {
        /* Half-open: just iris + pupil slit. */
        fill_ellipse(ix, iy, irx, iry, C_IRIS);
        fill_ellipse(ix, iy, irx * 2 / 3, iry / 2, C_PUPIL);
    }
}

/* Mouth variants. `my` is the mouth's centre row. The eyes are huge and reach
 * down to row 41, and the chin is the row-44 tip of the face, so a mouth only
 * has the rows around 39..43 and the columns cx-4..cx+4 to itself: anything
 * wider or lower draws over the chin outline or the hair behind it. Every
 * mouth is one pixel thick for the same reason - two rows read as a smear at
 * three screen pixels a cell - and none of them use teeth. */
static void draw_mouth(int cx, int my, muse_mode_t mode, float level,
                       float happy)
{
    switch (mode) {
    case MUSE_MODE_SPEAKING: {
        /* Opens with the audio level; tongue once it is wide open. Kept to
         * h <= 3 so the chin outline at row 44 stays clear of the mouth. */
        int h = 1 + (int)(level * 2);
        int w = 3 + (int)(level);
        ellipse_o(cx, my - 1, w, h, C_MOUTH, C_OUT);
        if (h > 2)
            fill_ellipse(cx, my + 1, w - 1, 1, C_TONGUE);
        break;
    }
    case MUSE_MODE_LISTENING:
        /* Small attentive "o". */
        ellipse_o(cx, my - 1, 2, 2, C_MOUTH, C_OUT);
        break;
    case MUSE_MODE_THINKING:
        /* Flat and short, pulled to one side. */
        for (int x = -3; x <= 3; x++)
            px(cx + x + 1, my - 1, C_OUT);
        break;
    case MUSE_MODE_ERROR:
        /* Worried: corners down, middle up. */
        for (int x = -4; x <= 4; x++)
            px(cx + x, my - 1 + (x * x) / 12, C_OUT);
        break;
    case MUSE_MODE_OFF:
        /* Soft closed smile. */
        for (int x = -3; x <= 3; x++)
            px(cx + x, my - (x * x) / 9, C_OUT);
        break;
    default:
        if (happy > 0.3f) {
            /* Open grin, tongue instead of teeth. */
            ellipse_o(cx, my - 1, 4, 3, C_MOUTH, C_OUT);
            fill_ellipse(cx, my + 1, 2, 1, C_TONGUE);
        } else {
            /* A thin closed smile, seven cells wide: at eight or more its ends
             * touch the eyes' outlines and the three merge into one line. */
            for (int x = -3; x <= 3; x++)
                px(cx + x, my - (x * x) / 9, C_OUT);
        }
        break;
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
    int bob = mode == MUSE_MODE_OFF ? 0 : (int)roundf(sinf(t * 2.2f) * 1.2f);
    /* Listening lean-in. */
    int lean = mode == MUSE_MODE_LISTENING ? -1 : 0;

    /* Thinking: pupils glance side to side. */
    int look_dx = 0, look_dy = 0;
    if (mode == MUSE_MODE_THINKING) {
        look_dx = (int)roundf(sinf(t * 3.1f) * 2.5f);
        look_dy = (int)roundf(cosf(t * 2.3f) * 1.0f);
    } else if (mode == MUSE_MODE_LISTENING) {
        look_dy = -1;
    }

    memset(s_fb, C_BG, sizeof(s_fb));
    if (fade <= 0.01f)
        return;

    int cx = 32;
    int cy = 28 + bob + lean;

    /* -- Dress (bottom of frame) --
     * The UI keeps the grid's bottom three rows clear for the mic level meter
     * it draws underneath and aligns the art's row 60 to that band
     * (ART_BLANK_ROWS in muse_ui.c), so the hem stays on row 60: a hem on row
     * 63 sits on top of the meter and hides its middle segments. The body sits
     * three rows higher to keep the dress's fourteen rows. */
    const int hem = 60;
    int shoulder = hem - 13;
    fill_rect(16, shoulder, 48, hem, C_DRESS);
    fill_rect(16, shoulder + 5, 48, shoulder + 8, C_STRIPE);
    fill_ellipse(cx, shoulder, 16, 6, C_DRESS);

    /* -- Face (fills the frame) -- */
    ellipse_o(cx, cy, 18, 16, C_SKIN, C_OUT);
    /* thin shading crescent at the jaw edge */
    fill_ellipse(cx + 9, cy + 9, 7, 5, C_SKIND);
    fill_ellipse(cx + 6, cy + 7, 8, 6, C_SKIN);

    /* -- Hair: the v8 dot matrix --
     * Drawn after the face, not before it: the matrix carries only the hair
     * that is visible in the photo, so the fringe, the temple edges and the
     * two long locks land on top of the skin, and nothing that lives behind
     * the face gets painted twice. v7's back-mass ellipses, lateral spines and
     * per-row fringe loop are all gone - the matrix replaces them.
     *
     * No neck either: the photo has the chin sitting straight on the dress and
     * the matrix stops at the jaw, so nothing covers the collar. */
    draw_hair(cy);

    /* -- Eyes (the huge signature ones) --
     * v8: the eyes sit on cy, not cy+3. In the reference the fringe's cut and the
     * top of the eye touch (the fringe hangs over the eye's upper edge); at cy+3
     * there were four rows of bare forehead and the face read long. On cy the
     * eye's outline top is cy-11, i.e. the row right under the fringe's cy-12, so
     * hair and eye meet - the reference's stack. */
    int ey = cy;
    draw_eye(cx - 11, ey, eye_open, look_dx, look_dy);
    draw_eye(cx + 11, ey, eye_open, look_dx, look_dy);

    /* v8: no brows. The reference has none - the fringe covers that band - and a
     * 1px brow at cy-11 read as a second line of hair. The expression rides on
     * the eye's own outline and the mouth. */

    /* -- Blush -- keyed to the face, not to the eye: v8 moved the eyes up two
     * rows, and a blush that follows ey landed on the eyes' lower corners. */
    fill_ellipse(cx - 16, cy + 11, 3, 2, C_BLUSH);
    fill_ellipse(cx + 16, cy + 11, 3, 2, C_BLUSH);

    /* -- Mouth -- */
    draw_mouth(cx, cy + 12, mode, level, happy);

    /* v8: no hair rim. v7 traced the hair's outer boundary in C_HAIRR so the
     * head would not vanish on a black screen, but that stroke drew the hair as
     * a separate outlined shape - the dome read as one piece, each flip as
     * another, and the notch between them as a line. The hair is one mass now:
     * where it meets the background it simply merges with it, and only the edge
     * against skin is visible, exactly as the reference reads. */

    /* -- State glow ring (subtle) -- */
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING ||
        mode == MUSE_MODE_SPEAKING) {
        /* Radius 26 +/- 1: the listening lean puts the head on row 27, so the
         * old 27 +/- 1.5 lost its top dot off the grid and flickered. */
        int gr = 26 + (int)roundf(sinf(t * 4.0f));
        for (int a = 0; a < 72; a++) {
            float ang = a * TAU / 72;
            px(cx + (int)roundf(cosf(ang) * gr), cy + (int)roundf(sinf(ang) * gr), C_GLOW);
        }
    }
    /* Happy: a heart floating up the left side. Seven by six pixels so it still
     * reads as a heart at three screen pixels a cell; the old two-by-two block
     * looked like a stray dot. Rows advance a third of a row per frame (~8
     * rows a second) instead of teleporting back every 1.8s. */
    if (happy > 0.4f) {
        static const uint8_t HEART[6] = {
            0x14,   /* ..X.X.. */
            0x3e,   /* .XXXXX. */
            0x7f,   /* XXXXXXX */
            0x3e,   /* .XXXXX. */
            0x1c,   /* ..XXX.. */
            0x08,   /* ...X... */
        };
        s_heart_y -= 0.32f;
        if (s_heart_y < 0.0f) {
            s_heart_y = 18.0f;
        }
        int hy = 2 + (int)s_heart_y, hx = 4;
        for (int r = 0; r < 6; r++) {
            for (int c = 0; c < 7; c++) {
                if (HEART[r] & (1u << (6 - c))) {
                    px(hx + c, hy + r, C_BLUSH);
                }
            }
        }
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
    /* Off: dim by punching BG holes. */
    if (mode == MUSE_MODE_OFF) {
        for (int i = 0; i < W * H; i++)
            if (s_fb[i] != C_BG && ((i * 7) & 3) == 0)
                s_fb[i] = C_BG;
    }
}
/*
 * Custom avatar: MaoMao (Buttercup), v3.
 *
 * 64x64 procedural pixel-art renderer for the muse-gadget-sdk avatar API,
 * drawn for the 360x360 round LCD of the ESP32-S3-Touch-LCD-1.85B. On that
 * board the grid is blown up 5x (320 px canvas centred on a 360 px screen),
 * so one cell is five screen pixels: every silhouette decision below is made
 * for that scale, and details that only work at 1-2 screen pixels are avoided.
 *
 * v3 is built from the 1998 Buttercup design, not from a generic "cute girl":
 *
 *   - Eyes. The pupil is NOT concentric. It is pressed against the inner edge
 *     and rides high, so the green iris only survives as a crescent along the
 *     outer-bottom and a white sliver sits outside that. Concentric eyes read
 *     as Bubbles; the offset is the whole character.
 *   - Brows. One cell thin, one cell above the lid, so they read as "almost
 *     joined to the eye" rather than as eyebrows floating on the forehead.
 *   - Pigtails. They flip UP and out from the crown. The v2 version pointed
 *     them down past the jaw, which read as sideburns, not as Buttercup.
 *   - Hairline. A W: a tall skin peak at the centre, a black point either side
 *     of it, then the hair sweeping up over the temples. That is what makes the
 *     forehead look like hers instead of like a bowl cut.
 *   - Eyes are clipped to the visible skin, so where the temple hair cuts in
 *     the eye simply stops. That "crammed under the hair" look is deliberate.
 *
 * Per-state animation: boot pop, idle bob + blink + gaze, listening lean-in,
 * thinking glance, speaking mouth, error wobble, off dim, pet "happy" reaction.
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
    C_OUT,      /* outline on skin / dress */
    C_HAIR,     /* hair body: near-black, blends into the black screen bg */
    C_HAIRL,    /* hair sheen */
    C_HAIRR,    /* hair rim: the only thing separating the head from the bg */
    C_SKIN,
    C_SKIND,    /* skin shadow */
    C_EYE,      /* sclera */
    C_IRIS,     /* Buttercup green */
    C_IRISD,    /* iris shade */
    C_PUPIL,
    C_SHINE,
    C_BROW,     /* thin lavender brow, as in the flat reference art */
    C_MOUTH,
    C_TONGUE,
    C_DRESS,
    C_STRIPE,
    C_BLUSH,
    C_GLOW,     /* state-tinted ring */
    C_COUNT,
};

static const uint32_t PAL_RGB[C_COUNT] = {
    [C_BG]     = 0x000000,
    [C_OUT]    = 0x2a2134,
    [C_HAIR]   = 0x0e0c13,
    [C_HAIRL]  = 0x282233,
    [C_HAIRR]  = 0x352c48,
    [C_SKIN]   = 0xffc9a0,
    [C_SKIND]  = 0xe9a87c,
    [C_EYE]    = 0xffffff,
    [C_IRIS]   = 0x62c257,
    [C_IRISD]  = 0x3f8f41,
    [C_PUPIL]  = 0x0a0a0e,
    [C_SHINE]  = 0xffffff,
    [C_BROW]   = 0x7d5f96,
    [C_MOUTH]  = 0x54242c,
    [C_TONGUE] = 0xe8707c,
    [C_DRESS]  = 0x4fbf5a,
    [C_STRIPE] = 0x101014,
    [C_BLUSH]  = 0xf29a9e,
    [C_GLOW]   = 0x4fbf5a,
};

/* State accent colours (also used for the glow ring). */
static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = 0x9fd0ff,
    [MUSE_MODE_IDLE]      = 0x4fbf5a,
    [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING]  = 0xe07bff,
    [MUSE_MODE_SPEAKING]  = 0x6ff0bf,
    [MUSE_MODE_ERROR]     = 0xff5c5c,
    [MUSE_MODE_OFF]       = 0x555566,
};

static uint8_t  s_fb[W * H];
static uint8_t  s_face[W * H];   /* 1 = visible skin, the clip for features */
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint32_t s_accent = 0x4fbf5a;
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
    s_pal[C_GLOW] = rgb565(s_accent);
    s_pal_dim[C_GLOW] = dim565(s_pal[C_GLOW]);
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/*
 * The head is the union of a big hair disc and a face ellipse that pokes out
 * below it, so the chin clears the hair by a row or two and the hair ends in a
 * sliver beside the jaw - the way the cartoon's hair stops above the chin.
 */
#define CX        32
#define HEAD_CY   23        /* hair disc centre */
#define HEAD_R    21
#define FACE_CY   30
#define FACE_RX   17
#define FACE_RY   16
#define EYE_CY    30
#define EYE_RX    7
#define EYE_RY    9
#define EYE_DX    9         /* eye centre offset from the face centre */
#define SHOULDER  48
#define HEM       60        /* the UI keeps the grid's bottom 3 rows for its meter */

/*
 * How many rows of hair hang below the face's own top edge, by |dx| from the
 * centre. Expressing the hairline as a depth under the ellipse (rather than an
 * absolute row) is what keeps it attached to the hair mass: an absolute table
 * dips below the ellipse's curve somewhere in the middle and leaves the points
 * floating on the forehead as two black "ears".
 *
 * Zero at the centre, so the forehead keeps a tall peach peak; 3 either side of
 * it, which is the black point that makes the hairline hers; then the temple
 * plunge that wraps the hair around the eyes.
 */
#define HAIRLINE_N 18
static const uint8_t HAIR_DEPTH[HAIRLINE_N] = {
    2, 2, 3, 4, 4, 3, 2, 2, 1, 1, 1, 1, 1, 3, 8, 11, 17, 25
};

/* ------------------------------------------------------------------ */
/* Draw primitives (framebuffer holds palette indices)                 */
/* ------------------------------------------------------------------ */

/* Non-NULL while drawing features that must stay on the visible skin. */
static const uint8_t *s_clip;

static void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        int i = y * W + x;
        if (s_clip && !s_clip[i])
            return;
        s_fb[i] = c;
    }
}

static uint8_t get_px(int x, int y)
{
    if ((unsigned)x < W && (unsigned)y < H)
        return s_fb[y * W + x];
    return C_BG;
}

static int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void fill_rect(int x0, int y0, int x1, int y1, uint8_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y, c);
}

static void fill_ellipse(int cx, int cy, int rx, int ry, uint8_t c)
{
    if (rx <= 0 || ry <= 0)
        return;
    for (int y = cy - ry; y <= cy + ry; y++)
        for (int x = cx - rx; x <= cx + rx; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                px(x, y, c);
        }
}

/* Outlined ellipse: fill then a 1-cell ring. */
static void ellipse_o(int cx, int cy, int rx, int ry, uint8_t fill, uint8_t out)
{
    fill_ellipse(cx, cy, rx + 1, ry + 1, out);
    fill_ellipse(cx, cy, rx, ry, fill);
}

/* A tapered hair horn: a chain of shrinking discs from (x0,y0) to (x1,y1). */
static void tuft(int x0, int y0, int x1, int y1, int w0)
{
    const int n = 22;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        int x = iround(x0 + (x1 - x0) * t);
        int y = iround(y0 + (y1 - y0) * t);
        int w = (int)(w0 * (1.0f - t) + 0.5f);
        if (w < 1) {
            px(x, y, C_HAIR);
        } else {
            fill_ellipse(x, y, w, w, C_HAIR);
        }
    }
}

/*
 * Turns the outer boundary of `body` pixels into `edge` pixels. Used for the
 * hair rim (the head would otherwise vanish into the black screen) and for the
 * jaw and dress outlines.
 */
static void rim(uint8_t body, uint8_t edge)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (s_fb[y * W + x] != body)
                continue;
            if (get_px(x - 1, y) == C_BG || get_px(x + 1, y) == C_BG ||
                get_px(x, y - 1) == C_BG || get_px(x, y + 1) == C_BG)
                s_fb[y * W + x] = edge;
        }
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

/* Idle behaviour: blink and gaze, driven off pose.t like the stock renderer. */
static float s_next_blink = 2.0f;
static float s_blink_start = -10.0f;
static float s_next_gaze = 1.0f;
static float s_gx, s_gy;        /* current gaze, -1..1 */
static float s_tgx, s_tgy;      /* target gaze */
static float s_heart_y = 20.0f;

/* Deterministic pseudo-random in 0..1, so the idle has some life to it. */
static uint32_t s_rng = 0x2545f491u;
static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / (float)0x1000000;
}

static void gaze_update(const muse_pose_t *p, float dt)
{
    if (p->t >= s_next_gaze) {
        s_next_gaze = p->t + 1.4f + frand() * 2.6f;
        if (frand() < 0.4f) {
            s_tgx = 0;
            s_tgy = 0;
        } else {
            s_tgx = frand() * 2 - 1;
            s_tgy = (frand() * 2 - 1) * 0.5f;
        }
    }
    float tgx = s_tgx, tgy = s_tgy;
    switch (p->mode) {
    case MUSE_MODE_LISTENING:
        tgx = 0;
        tgy = 0;
        break;
    case MUSE_MODE_THINKING:
        /* Glance up and away, the way she does when she is not listening. */
        tgx = 0.9f * sinf(p->mode_t * 1.2f) + 0.2f;
        tgy = -1.0f;
        break;
    case MUSE_MODE_SPEAKING:
        tgx *= 0.35f;
        tgy = 0;
        break;
    default:
        break;
    }
    float k = 1.0f - expf(-dt * 12.0f);
    s_gx += (tgx - s_gx) * k;
    s_gy += (tgy - s_gy) * k;
}

/* ------------------------------------------------------------------ */
/* Body                                                                */
/* ------------------------------------------------------------------ */

/* `arm` lifts both stubs by that many rows; 0 is the resting pose. */
static void draw_body(int cx, int chin, float arm)
{
    /* Neck, mostly hidden behind the chin. */
    fill_rect(cx - 3, chin - 1, cx + 3, SHOULDER, C_SKIND);

    /*
     * Dress: a narrow block, roughly half the head's width, like the cartoon's
     * tiny body under an oversized head. The top two rows are bevelled so the
     * shoulders are not a hard 90 degrees.
     */
    const int half = 10;
    for (int y = SHOULDER; y <= HEM; y++) {
        int inset = y == SHOULDER ? 2 : (y == SHOULDER + 1 ? 1 : 0);
        fill_rect(cx - half + inset, y, cx + half - inset, y, C_DRESS);
    }
    /* Black stripe across the middle. */
    fill_rect(cx - half, SHOULDER + 5, cx + half, SHOULDER + 7, C_STRIPE);

    /* Arms: short peach stubs overlapping the dress edge, so they read as
     * attached rather than as two floating ovals beside it. */
    int ay = SHOULDER + 4 - iround(arm);
    ellipse_o(cx - half + 1, ay + 3, 3, 5, C_SKIN, C_OUT);
    ellipse_o(cx + half - 1, ay + 3, 3, 5, C_SKIN, C_OUT);
}

/* ------------------------------------------------------------------ */
/* Head                                                                */
/* ------------------------------------------------------------------ */

static void draw_hair(int cx, int cy)
{
    int d = cy - HEAD_CY;
    fill_ellipse(cx, cy, HEAD_R, HEAD_R, C_HAIR);

    /*
     * Pigtails: two horns per side, the lower one shorter, both swept up and
     * out. Tips stay inside radius 35 of the screen centre - beyond that the
     * round bezel eats them.
     */
    tuft(cx - 16, cy - 11, cx - 23, cy - 17, 4);  /* left, long */
    tuft(cx - 19, cy - 4, cx - 26, cy - 9, 3);    /* left, short */
    tuft(cx + 16, cy - 11, cx + 23, cy - 17, 4);  /* right, long */
    tuft(cx + 19, cy - 4, cx + 26, cy - 9, 3);    /* right, short */
    /* A small crown tuft, so the top of the head is not a perfect circle. */
    tuft(cx + 3, cy - 19, cx + 8, cy - 22, 2);
    (void)d;
}

static void draw_face(int cx, int fy)
{
    memset(s_face, 0, sizeof(s_face));

    for (int x = cx - FACE_RX; x <= cx + FACE_RX; x++) {
        int dx = x - cx;
        int adx = dx < 0 ? -dx : dx;
        float u = (float)dx / FACE_RX;
        int span = (int)(FACE_RY * sqrtf(1.0f - u * u) + 0.5f);
        if (span <= 0)
            continue;
        int top = fy - span, bot = fy + span;
        int depth = adx < HAIRLINE_N ? HAIR_DEPTH[adx] : 30;
        int hl = top + depth;
        for (int y = top; y <= bot; y++) {
            int i = y * W + x;
            if (y <= hl) {
                s_fb[i] = C_HAIR;       /* the bangs are cut out of the face */
            } else {
                s_fb[i] = C_SKIN;
                s_face[i] = 1;
            }
        }
    }

    /* Shade the jaw and the temples so the head is not a flat peach blob. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (!s_face[i])
                continue;
            int dx = x - cx, dy = y - fy;
            if (dy > FACE_RY - 4 && dx * dx > 60)
                s_fb[i] = C_SKIND;
        }
    }
}

/*
 * One eye. `inward` is +1 for the left eye and -1 for the right: everything
 * that makes the eye hers is pushed along it.
 */
static void draw_eye(int cx, int cy, int inward, float open, int ldx, int ldy, bool happy)
{
    if (open < 0.2f) {
        /* Shut: a thick lid line. Happy arcs up, a blink sags a little. */
        for (int x = -6; x <= 6; x++) {
            int y = happy ? cy + 2 - (x * x) / 14 : cy + 1 + (x * x) / 20;
            px(cx + x, y, C_OUT);
            px(cx + x, y + 1, C_OUT);
        }
        return;
    }

    int rx = EYE_RX;
    int ry = (int)(EYE_RY * open + 0.5f);
    if (ry < 4)
        ry = 4;

    /* Sclera and its outline. The clip trims both where the hair cuts in. */
    ellipse_o(cx, cy, rx, ry, C_EYE, C_OUT);

    /*
     * Three discs, each a cell smaller than the one before, and each pushed a
     * different way. The white stays a thin rim because the iris only loses one
     * cell of radius; the green survives as a crescent because the pupil is the
     * same size as the iris but sits two cells further in. That offset pair is
     * the whole eye - draw them concentric and she turns into Bubbles.
     */
    int grx = rx - 2, gry = ry - 2;
    int prx = rx - 2, pry = ry - 2;
    if (grx < 2) grx = 2;
    if (gry < 2) gry = 2;

    fill_ellipse(cx - inward + ldx, cy + 1 + ldy, grx, gry, C_IRIS);
    fill_ellipse(cx + inward * 2 + ldx, cy - 1 + ldy, prx, pry, C_PUPIL);

    if (open > 0.45f) {
        /* The big round highlight sits upper-inner, inside the black. */
        fill_ellipse(cx + inward * 2 + ldx, cy - 4 + ldy, 2, 2, C_SHINE);
    }
}

/* A one-cell brow that follows the lid, `tilt` rows lower at the inner end. */
static void draw_brow(int cx, int cy, int inward, int tilt)
{
    for (int x = -5; x <= 3; x++) {
        int y = cy - EYE_RY - 1 + (x * x) / 30;
        y += (x * inward * tilt) / 6;
        px(cx + x, y, C_BROW);
    }
}

/* Mouth variants. `my` is the centre row; the chin is 4 rows below it. */
static void draw_mouth(int cx, int my, muse_mode_t mode, float level, float happy)
{
    switch (mode) {
    case MUSE_MODE_SPEAKING: {
        int h = 1 + (int)(level * 2);
        int w = 3 + (int)(level);
        ellipse_o(cx, my - 1, w, h, C_MOUTH, C_OUT);
        if (h > 2)
            fill_ellipse(cx, my + 1, w - 1, 1, C_TONGUE);
        break;
    }
    case MUSE_MODE_LISTENING:
        ellipse_o(cx, my - 1, 2, 2, C_MOUTH, C_OUT);
        break;
    case MUSE_MODE_THINKING:
        /* Flat and pushed to one side - the smirk she does while scheming. */
        for (int x = -2; x <= 4; x++)
            px(cx + x, my - 1, C_OUT);
        break;
    case MUSE_MODE_ERROR:
        /* Worried: corners down, middle up. */
        for (int x = -4; x <= 4; x++)
            px(cx + x, my - 1 + (x * x) / 12, C_OUT);
        break;
    case MUSE_MODE_OFF:
        for (int x = -3; x <= 3; x++)
            px(cx + x, my - (x * x) / 9, C_OUT);
        break;
    default:
        if (happy > 0.3f) {
            /* Open grin with the tongue showing. */
            ellipse_o(cx, my - 1, 4, 3, C_MOUTH, C_OUT);
            fill_ellipse(cx, my + 1, 2, 1, C_TONGUE);
        } else {
            /* Closed smile, nine cells wide with the corners two rows down. */
            for (int x = -4; x <= 4; x++)
                px(cx + x, my - (x * x) / 8, C_OUT);
        }
        break;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_init;
    static float s_last_t = -1;
    if (!s_init) { init_pal(); s_init = true; }

    s_accent = muse_pixel_accent(p->mode);
    s_pal[C_GLOW] = rgb565(s_accent);
    s_pal_dim[C_GLOW] = dim565(s_pal[C_GLOW]);

    muse_mode_t mode = p->mode;
    float t = p->t;
    float happy = clampf(p->happy, 0, 1);
    float level = clampf(p->level, 0, 1);
    float dt = s_last_t >= 0 ? clampf(t - s_last_t, 0.0f, 0.2f) : 0.04f;
    s_last_t = t;

    /* Boot: the whole figure fades in over ~1s. */
    float fade = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.0f, 0, 1) : 1.0f;

    /* Blink schedule. */
    float blink = 0;
    if (mode != MUSE_MODE_OFF && mode != MUSE_MODE_ERROR) {
        if (t >= s_next_blink) {
            s_blink_start = t;
            s_next_blink = t + (frand() < 0.2f ? 0.3f : 2.2f + frand() * 2.4f);
        }
        float bt = (t - s_blink_start) / 0.14f;
        if (bt >= 0 && bt <= 1)
            blink = 1.0f - fabsf(bt * 2 - 1);
    }
    gaze_update(p, dt);

    /* Gentle idle bob; frozen when off. */
    int bob = mode == MUSE_MODE_OFF ? 0 : iround(sinf(t * 2.2f) * 1.2f);
    int lean = mode == MUSE_MODE_LISTENING ? -1 : 0;
    if (mode == MUSE_MODE_ERROR)
        lean = iround(sinf(t * 18.0f) * (p->mode_t < 0.6f ? 1.0f : 0.0f));
    int hop = happy > 0 ? iround(fabsf(sinf(t * 9.0f)) * 3.0f * happy) : 0;

    memset(s_fb, C_BG, sizeof(s_fb));
    if (fade <= 0.01f)
        return;

    int cx = CX + lean;
    int cy = HEAD_CY + bob - hop;
    int fy = FACE_CY + bob - hop;
    int chin = fy + FACE_RY;

    s_clip = NULL;
    draw_body(cx, chin, happy > 0.2f ? 3.0f : 0.0f);
    draw_hair(cx, cy);
    draw_face(cx, fy);

    /* ---- face ---- */
    s_clip = s_face;
    int ex = cx - EYE_DX, ex2 = cx + EYE_DX;
    float open = 1.0f - blink;
    int ldx = 0, ldy = 0;
    if (mode != MUSE_MODE_OFF && mode != MUSE_MODE_ERROR) {
        ldx = (int)floorf(clampf(s_gx, -1, 1) + 0.5f);
        ldy = (int)floorf(clampf(s_gy, -1, 1) + 0.5f);
    }
    bool happy_eyes = happy > 0.3f && mode != MUSE_MODE_ERROR;
    int brow_tilt = 0;

    switch (mode) {
    case MUSE_MODE_LISTENING:
        open = 1.0f;
        brow_tilt = -1;         /* brows up: paying attention */
        break;
    case MUSE_MODE_THINKING:
        brow_tilt = -1;
        break;
    case MUSE_MODE_ERROR:
        open = 0.72f;
        brow_tilt = 1;          /* brows down at the nose: cross */
        ldy = 0;
        break;
    case MUSE_MODE_OFF:
        open = 0.0f;
        break;
    default:
        break;
    }

    int ey = fy + 1;
    draw_eye(ex, ey, +1, happy_eyes ? 0.0f : open, ldx, ldy, happy_eyes);
    draw_eye(ex2, ey, -1, happy_eyes ? 0.0f : open, ldx, ldy, happy_eyes);
    if (!happy_eyes && open > 0.25f) {
        draw_brow(ex, ey, +1, brow_tilt);
        draw_brow(ex2, ey, -1, brow_tilt);
    }

    /* Blush, tucked under the eyes on the cheeks. */
    int by = fy + 11;
    fill_ellipse(cx - 12, by, 2, 1, C_BLUSH);
    fill_ellipse(cx + 12, by, 2, 1, C_BLUSH);

    draw_mouth(cx, fy + 13, mode, level, happy);
    s_clip = NULL;

    /* ---- silhouette ---- */
    rim(C_HAIR, C_HAIRR);
    rim(C_SKIN, C_OUT);

    /* ---- state effects ---- */
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING ||
        mode == MUSE_MODE_SPEAKING) {
        int gr = 28 + iround(sinf(t * 4.0f));
        for (int a = 0; a < 72; a++) {
            float ang = a * TAU / 72;
            int x = cx + iround(cosf(ang) * gr);
            int y = cy + iround(sinf(ang) * gr);
            if (get_px(x, y) == C_BG)
                px(x, y, C_GLOW);
        }
    }
    if (happy > 0.4f) {
        static const uint8_t HEART[6] = {
            0x14, 0x3e, 0x7f, 0x3e, 0x1c, 0x08,
        };
        s_heart_y -= 0.32f;
        if (s_heart_y < 0.0f)
            s_heart_y = 20.0f;
        int hy = 2 + (int)s_heart_y, hx = 4;
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 7; c++)
                if (HEART[r] & (1u << (6 - c)))
                    px(hx + c, hy + r, C_BLUSH);
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

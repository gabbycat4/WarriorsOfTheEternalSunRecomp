/*
 * wotes_video.c: WotES presentation adapter (Phase 3/4: high refresh rate).
 *
 * Plugs into the engine's host-only interpolation path (GameVideo in
 * runner/game_video.h): with "High refresh rate" on (Esc -> Mods, or
 * settings.ini [video] presentation_rate = -1 for the display's rate), the
 * runner issues 2-4 pictures per 59.94 Hz game tick and calls interpolate()
 * with phase 0 (previous frame) .. 1 (latest frame) for each.
 *
 * Method: at the end of every real frame keep a COPY of the VDP state
 * (VRAM/CRAM/VSRAM/registers: what that frame was drawn from) and the real
 * picture. In-between pictures re-render a copy with the engine's own renderer
 * (gvdp_render_scanline), so priorities, palettes, the window plane and the
 * side panel stay pixel-exact; Phase 4 moves camera/sprites on that copy.
 *
 * Step 4.1: self-check. Once a second (and while WOTES_VIDEO_MOTION=0) the
 * latest frame's copy is rendered unmoved and compared with the real picture.
 * Step 4.2 (this version): motion. The camera (plane scroll) is interpolated
 * between the previous and latest frame; sprites become tracked objects
 * (matched by tile group + palette + nearest position) moving in WORLD
 * coordinates (screen + camera). Each object learns its step period: 1-px-per-
 * frame walkers ease over one frame, battle characters that hop 8 px every 9
 * frames glide over those 9 frames (Lagoon's step interpolation; up to one
 * step of visual delay for that object, input unaffected). Jumps > 16 px in
 * one frame (menu pointer, teleports) are cuts, and
 * new/unmatched sprites are shown where the game put them.
 * Step 4.3: quarter-pixel pictures. With the engine's interpolate_scaled hook
 * the in-between pictures are drawn at 4x (WOTES_VIDEO_SCALE=2..4, 1 = off),
 * so a 1-px-per-frame camera moves 1/4 px per 240 Hz picture instead of
 * jumping a whole pixel every other picture. Planes are captured once at the
 * whole-pixel camera; sprites are captured in groups that share the same
 * quarter-pixel fraction; then layers are shifted by 0 or 1 native pixel per
 * sub-pixel column/row and composed with the VDP's own priority rules
 * (gvdp_compose_span). The window plane (side panel) never moves.
 * WOTES_VIDEO_LOG=1 logs a line once a second; WOTES_VIDEO_MOTION=0 disables
 * motion (pure self-check mode).
 * Never touches guest state; never runs when HD presentation is active
 * (the runner disables interpolation for the HD texture).
 */
#include "game_video.h"
#include "wotes_video.h"
#include "genesis_machine.h"
#include "genesis_vdp.h"
#include "genesis_dac.h"
#include "genesis_vdp_compose.h"

#include <limits.h>
#include <math.h>
#include <time.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WV_W 320
#define WV_H 240

typedef struct {
    GVDP     vdp;                               /* state the frame was drawn from */
    uint32_t real[WV_W * WV_H];                 /* the real picture (for self-check) */
    uint32_t host[GVDP_HOST_PALETTE_SIZE];      /* extra host colours, if any */
    int      width, height, complete;
    unsigned serial;
} WVFrame;

static int      s_enabled;
static WVFrame *s_frames;          /* [2], allocated on first use */
static int      s_latest = -1;     /* index of the newest complete frame */
static int      s_build  = 0;      /* index being captured */
static unsigned s_serial;
static GVDP     s_work;            /* scratch copy rendered for a picture */
static int      s_log = -1;
static unsigned long long s_checks, s_check_fail, s_pictures;
static unsigned s_last_log_serial;

static void wv_init(void)
{
    if (!s_frames) s_frames = (WVFrame *)calloc(2, sizeof *s_frames);
    if (s_log < 0) { const char *e = getenv("WOTES_VIDEO_LOG"); s_log = e && *e == '1'; }
}

static void wotes_presentation_configure(int enabled)
{
    s_enabled = enabled;
    wv_init();
    s_latest = -1;
    fprintf(stderr, "[WOTES-VIDEO] high refresh rate %s (step 4.3: motion, quarter-pixel pictures)\n",
            enabled ? "ON" : "off");
}

static void wotes_presentation_reset(void) { s_latest = -1; }

static void wotes_presentation_capture(const struct GVDP *vdp, int line,
                                       const uint32_t *image, int width, int native_w,
                                       const struct GVDPRasterRow *raster)
{
    (void)native_w;
    wv_init();
    if (!s_frames || line < 0 || line >= WV_H) return;
    WVFrame *f = &s_frames[s_build];
    if (line == 0) {
        f->complete = 0;
        f->width = width;
        memset(f->host, 0, sizeof f->host);
        if (raster) memcpy(f->host, raster->host_palette, sizeof f->host);
    }
    if (width != f->width || width > WV_W) { f->width = -1; return; }   /* unsupported */
    memcpy(&f->real[(size_t)line * WV_W], image, (size_t)width * sizeof *image);
    int h = gvdp_output_height(vdp);
    if (line == h - 1 && f->width > 0) {
        f->vdp = *vdp;              /* the state this frame was drawn from */
        f->height = h;
        f->complete = 1;
        f->serial = ++s_serial;
        s_latest = s_build;
        s_build ^= 1;
    }
}

/* Same output palette the engine builds (genesis_machine.c colour_cb):
 * 0-63 normal, 64-127 shadow, 128-191 highlight, bar sentinel, host colours. */
static void wv_palette(const GVDP *v, const uint32_t *host, uint32_t pal[GVDP_OUTPUT_PALETTE_SIZE])
{
    for (int i = 0; i < GVDP_OUTPUT_PALETTE_SIZE; i++) pal[i] = 0xFF000000u;
    for (int i = 0; i < GVDP_CRAM_ENTRIES; i++) {
        pal[i]                          = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_NORMAL);
        pal[i + GVDP_PALETTE_SHADOW]    = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_SHADOW);
        pal[i + GVDP_PALETTE_HIGHLIGHT] = genesis_dac_cram_to_argb(v->cram[i], GENESIS_DAC_HIGHLIGHT);
    }
    for (int i = 0; i < GVDP_HOST_PALETTE_SIZE; i++)
        if (host[i]) pal[GVDP_HOST_PALETTE_BASE + i] = host[i];
}

/* Render a VDP state into ARGB rows. Returns 0 if the geometry is unexpected. */
static int wv_render(GVDP *v, const uint32_t *host, uint32_t *image, int stride, int height, int width)
{
    uint32_t pal[GVDP_OUTPUT_PALETTE_SIZE];
    uint8_t idx[GVDP_MAX_WIDTH];
    wv_palette(v, host, pal);
    for (int y = 0; y < height; y++) {
        int n = gvdp_render_scanline(v, y, idx);
        if (n != width) return 0;
        uint32_t *row = image + (size_t)y * stride;
        for (int x = 0; x < width; x++) row[x] = pal[idx[x]];
    }
    return 1;
}


/* ---------------------------------------------------------------- motion -- */
#define WV_MAX_SPR 80
#define WV_MAX_OBJ 96

typedef struct { int x, y, w, h; uint16_t attr; int entry; } WVSprite;  /* screen px */

typedef struct {
    int      used, seen;           /* seen = matched in the latest frame */
    int      key;                  /* tile group | palette << 12 */
    int      key2;                 /* palette | size: fallback match across animation frames */
    int      wx, wy;               /* world position in the latest frame */
    double   fx, fy;               /* glide start (world) */
    int      since;                /* frames since the last step */
    int      period;               /* learned step period (frames) */
    int      gaps[4], ngaps;
    int      slot;                 /* SAT entry in the latest frame */
} WVObj;

static WVObj  s_obj[WV_MAX_OBJ];
static int    s_cam_hs[2], s_cam_vs[2];  /* [0] previous, [1] latest raw scroll (A plane) */
static int    s_have_prev_cam;
static int    s_motion = -1;
static unsigned s_motion_serial;         /* latest frame the tracker has processed */

static uint16_t rd16(const GVDP *v, uint32_t a) { a &= 0xFFFE; return (uint16_t)(v->vram[a] << 8 | v->vram[a + 1]); }
static void     wr16(GVDP *v, uint32_t a, uint16_t d) { a &= 0xFFFE; v->vram[a] = (uint8_t)(d >> 8); v->vram[a + 1] = (uint8_t)d; }
static int wrap10(int d) { d &= 0x3FF; return d >= 0x200 ? d - 0x400 : d; }

static uint32_t sat_base(const GVDP *v)  { int h40 = (v->reg[12] & 0x81) != 0; return ((uint32_t)v->reg[5] & (h40 ? 0x7Eu : 0x7Fu)) << 9; }
static uint32_t hs_base(const GVDP *v)   { return ((uint32_t)v->reg[13] & 0x3Fu) << 10; }

/* Sprites in link order, screen coordinates. */
static int wv_sprites(const GVDP *v, WVSprite *out)
{
    int h40 = (v->reg[12] & 0x81) != 0, max = h40 ? 80 : 64, idx = 0, n = 0;
    uint32_t sat = sat_base(v);
    while (n < max) {
        uint32_t e = sat + (uint32_t)idx * 8u;
        uint16_t y = rd16(v, e), sl = rd16(v, e + 2), at = rd16(v, e + 4), x = rd16(v, e + 6);
        out[n].x = (x & 0x1FF) - 128; out[n].y = (y & 0x3FF) - 128;
        out[n].w = (((sl >> 10) & 3) + 1) * 8; out[n].h = (((sl >> 8) & 3) + 1) * 8;
        out[n].attr = at; out[n].entry = idx;
        n++;
        idx = sl & 0x7F;
        if (!idx || idx >= max) break;
    }
    return n;
}

static int obj_period(const WVObj *o)
{
    if (o->ngaps < 2) return 1;
    int a[4], n = o->ngaps < 4 ? o->ngaps : 4;
    for (int i = 0; i < n; i++) a[i] = o->gaps[i];
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && a[j - 1] > a[j]; j--) { int t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    int p = a[n / 2];
    return p < 1 ? 1 : p > 12 ? 12 : p;
}

/* Displayed world position of an object at picture phase p (0..1). */
static void obj_display(const WVObj *o, double p, double *x, double *y)
{
    double prog = ((double)o->since + p) / (double)o->period;
    if (prog > 1.0) prog = 1.0;
    if (prog < 0.0) prog = 0.0;
    *x = o->fx + (o->wx - o->fx) * prog;
    *y = o->fy + (o->wy - o->fy) * prog;
}

/* Advance the tracker to the latest completed frame. */
static void wv_track(const WVFrame *f)
{
    const GVDP *v = &f->vdp;
    int hs = rd16(v, hs_base(v)) & 0x3FF, vs = v->vsram[0] & 0x3FF;
    if (s_have_prev_cam) { s_cam_hs[0] = s_cam_hs[1]; s_cam_vs[0] = s_cam_vs[1]; }
    else { s_cam_hs[0] = hs; s_cam_vs[0] = vs; }
    s_cam_hs[1] = hs; s_cam_vs[1] = vs; s_have_prev_cam = 1;

    WVSprite spr[WV_MAX_SPR];
    int n = wv_sprites(v, spr);
    int match[WV_MAX_SPR];
    for (int i = 0; i < WV_MAX_OBJ; i++) s_obj[i].seen = 0;
    for (int k = 0; k < n; k++) match[k] = -1;
    /* Pass 1: same tile group + palette, nearest. Pass 2 (animation frame
     * changed the tile group): same palette + size, nearest. */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < n; k++) {
            if (match[k] >= 0) continue;
            int key  = ((spr[k].attr & 0x7FF) >> 3) | (((spr[k].attr >> 13) & 3) << 12);
            int key2 = (((spr[k].attr >> 13) & 3) << 12) | (spr[k].w << 6) | spr[k].h;
            int wx = spr[k].x - hs, wy = spr[k].y + vs;
            int best = -1, bd = 1 << 30;
            for (int i = 0; i < WV_MAX_OBJ; i++) {
                WVObj *o = &s_obj[i];
                if (!o->used || o->seen) continue;
                if (pass == 0 ? o->key != key : o->key2 != key2) continue;
                int dx = wrap10(wx - o->wx), dy = wrap10(wy - o->wy), d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0 && bd <= 48 * 48) { match[k] = best; s_obj[best].seen = 1; }
        }
    }
    for (int i = 0; i < WV_MAX_OBJ; i++) s_obj[i].seen = 0;
    for (int k = 0; k < n; k++) {
        int key  = ((spr[k].attr & 0x7FF) >> 3) | (((spr[k].attr >> 13) & 3) << 12);
        int key2 = (((spr[k].attr >> 13) & 3) << 12) | (spr[k].w << 6) | spr[k].h;
        int wx = spr[k].x - hs, wy = spr[k].y + vs;          /* world = screen + camera */
        int best = match[k];
        WVObj *o;
        if (best < 0) {
            for (best = 0; best < WV_MAX_OBJ && s_obj[best].used; best++) {}
            if (best >= WV_MAX_OBJ) continue;
            o = &s_obj[best];
            memset(o, 0, sizeof *o);
            o->used = 1; o->wx = wx; o->wy = wy; o->fx = wx; o->fy = wy;
            o->since = 1000; o->period = 1;
        } else {
            o = &s_obj[best];
            int dx = wrap10(wx - o->wx), dy = wrap10(wy - o->wy);
            if (dx > 16 || dx < -16 || dy > 16 || dy < -16) {
                /* A jump (menu pointer between slots, teleport): show it at
                 * once, no glide, and don't learn a period from it. */
                o->wx = wx; o->wy = wy; o->fx = wx; o->fy = wy; o->since = 1000;
            } else if (dx || dy) {
                /* A step: glide from where it is being shown now (end of the
                 * previous frame) to the new position. */
                double cx, cy; obj_display(o, 1.0, &cx, &cy);
                if (o->since < 1000) {
                    for (int g = 3; g > 0; g--) o->gaps[g] = o->gaps[g - 1];
                    o->gaps[0] = o->since + 1;
                    if (o->ngaps < 4) o->ngaps++;
                }
                o->period = obj_period(o);
                o->fx = cx + (double)(wx - (o->wx + dx));   /* same plane wrap as the new wx */
                o->fy = cy + (double)(wy - (o->wy + dy));
                o->wx = wx; o->wy = wy;
                o->since = 0;
            } else if (o->since < 1000) {
                o->since++;
            }
        }
        o->key = key; o->key2 = key2;
        o->seen = 1; o->slot = spr[k].entry;
    }
    for (int i = 0; i < WV_MAX_OBJ; i++) if (s_obj[i].used && !s_obj[i].seen) s_obj[i].used = 0;
}

/* Move camera and sprites on the scratch copy to their positions at phase p. */
static void wv_apply_motion(GVDP *w, double p)
{
    /* Camera: previous -> latest over this frame. */
    int dhs = wrap10(s_cam_hs[1] - s_cam_hs[0]), dvs = wrap10(s_cam_vs[1] - s_cam_vs[0]);
    int ohs = (int)((dhs) * (p - 1.0) + (dhs * (p - 1.0) < 0 ? -0.5 : 0.5));   /* offset from latest */
    int ovs = (int)((dvs) * (p - 1.0) + (dvs * (p - 1.0) < 0 ? -0.5 : 0.5));
    if (dhs > 32 || dhs < -32) ohs = 0;                   /* cut, not a pan */
    if (dvs > 32 || dvs < -32) ovs = 0;
    if (ohs) {
        uint32_t base = hs_base(w); int mode = w->reg[11] & 3, step = mode == 3 ? 4 : mode == 2 ? 32 : 0;
        int lines = step ? (mode == 3 ? 224 : 28) : 1;
        for (int i = 0; i < lines; i++) for (int plane = 0; plane < 2; plane++) {
            uint32_t a = base + (uint32_t)i * (uint32_t)step + (uint32_t)plane * 2u;
            uint16_t d = rd16(w, a);
            wr16(w, a, (uint16_t)((d & 0xFC00) | ((d + ohs) & 0x3FF)));
        }
    }
    if (ovs) {
        int cols = (w->reg[11] & 4) ? GVDP_VSRAM_ENTRIES : 2;
        for (int i = 0; i < cols; i++) w->vsram[i] = (uint16_t)((w->vsram[i] & 0xFC00) | ((w->vsram[i] + ovs) & 0x3FF));
    }
    int cam_hs = s_cam_hs[1] + ohs, cam_vs = s_cam_vs[1] + ovs;   /* displayed camera */

    /* Sprites: world glide, then back to screen with the displayed camera. */
    uint32_t sat = sat_base(w);
    for (int i = 0; i < WV_MAX_OBJ; i++) {
        WVObj *o = &s_obj[i];
        if (!o->used || !o->seen) continue;     /* newcomers: prog 1 = where the game put them */
        double x, y; obj_display(o, p, &x, &y);
        int sx = (int)(x + (x < 0 ? -0.5 : 0.5)) + cam_hs;
        int sy = (int)(y + (y < 0 ? -0.5 : 0.5)) - cam_vs;
        uint32_t e = sat + (uint32_t)o->slot * 8u;
        uint16_t oy = rd16(w, e), ox = rd16(w, e + 6);
        if ((ox & 0x1FF) == 0) continue;        /* x = 0 is a line mask, not a picture */
        wr16(w, e,     (uint16_t)((oy & 0xFC00) | ((sy + 128) & 0x3FF)));
        wr16(w, e + 6, (uint16_t)((ox & 0xFE00) | ((sx + 128) & 0x1FF)));
    }
}

static void wv_motion_env(void)
{
    if (s_motion < 0) { const char *e = getenv("WOTES_VIDEO_MOTION"); s_motion = !(e && *e == '0'); }
}

static void wv_advance(const WVFrame *f)
{
    if (f->serial != s_motion_serial) {                  /* a new real frame */
        if (f->serial != s_motion_serial + 1) s_have_prev_cam = 0;   /* missed frames: no glide across */
        wv_track(f);
        s_motion_serial = f->serial;
    }
}

static int wotes_interpolate(double phase, uint32_t *image, int stride, int height)
{
    if (!s_enabled || !s_frames || s_latest < 0) return 0;
    WVFrame *f = &s_frames[s_latest];
    if (!f->complete || f->width <= 0 || f->height != height) return 0;
    wv_motion_env();
    s_pictures++;
    wv_advance(f);

    /* Self-check once a second (every picture when motion is off). */
    int check = !s_motion || f->serial - s_last_log_serial >= 60;
    if (check) {
        s_work = f->vdp;
        if (!wv_render(&s_work, f->host, image, stride, height, f->width)) return 0;
        unsigned long long diff = 0;
        for (int y = 0; y < height; y++) {
            const uint32_t *a = image + (size_t)y * stride, *b = &f->real[(size_t)y * WV_W];
            for (int x = 0; x < f->width; x++) diff += a[x] != b[x];
        }
        s_checks++;
        if (diff) s_check_fail++;
        if (f->serial - s_last_log_serial >= 60) {
            s_last_log_serial = f->serial;
            if (s_log) {
                int moving = 0, glide = 0;
                for (int i = 0; i < WV_MAX_OBJ; i++) if (s_obj[i].used && s_obj[i].seen) {
                    moving += s_obj[i].since < s_obj[i].period; glide += s_obj[i].period > 1; }
                fprintf(stderr, "[WOTES-VIDEO] frame %u self-check: %llu px differ (%llu/%llu checks differed) "
                        "cam %+d,%+d objects moving %d gliding(period>1) %d\n",
                        f->serial, diff, s_check_fail, s_checks,
                        wrap10(s_cam_hs[1] - s_cam_hs[0]), wrap10(s_cam_vs[1] - s_cam_vs[0]), moving, glide);
            }
        }
        if (diff) return 0;            /* renderer disagrees: show the real picture */
        if (!s_motion) return 1;
    }

    s_work = f->vdp;
    wv_apply_motion(&s_work, phase);
    return wv_render(&s_work, f->host, image, stride, height, f->width);
}

/* ------------------------------------------------ quarter pixels (4.3) -- */
#define WV_MAX_GRP 4

typedef struct { int fx, fy, rank; } WVGroup;   /* sprites sharing one sub-pixel fraction */

static int      s_scale = -1;
static uint8_t  s_pa[WV_H][WV_W], s_pb[WV_H][WV_W];            /* planes at the whole-pixel camera */
static uint8_t  s_gs[WV_MAX_GRP][WV_H][WV_W];                  /* sprite colour per group */
static uint8_t  s_gf[WV_MAX_GRP][WV_H][WV_W];                  /* 1 opaque, 2 high, 4 shadow op, 8 highlight op */
static uint8_t  s_bd[WV_H], s_sm[WV_H];
static GVDPRasterRow s_row;
static uint32_t s_native[WV_W * WV_H];
static double   s_t_sum, s_t_max;
static unsigned s_t_n, s_t_groups;
static unsigned long long s_fallbacks, s_qchecks, s_qcheck_fail, s_qdiff_last;
static int      s_qcheck_pending;

static int fdiv(int a, int d) { return a >= 0 ? a / d : -((-a + d - 1) / d); }
static int qround(double v)   { return (int)floor(v + 0.5); }
static double wv_now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* Nearest-neighbour copy of the real picture (fallback, and check failures). */
static int wv_scale_real(const WVFrame *f, uint32_t *image, int stride, int height, int S)
{
    int W = f->width;
    for (int y = 0; y < height; y++) {
        const uint32_t *src = &f->real[(size_t)y * WV_W];
        uint32_t *d0 = image + (size_t)(y * S) * stride;
        for (int x = 0; x < W; x++) for (int k = 0; k < S; k++) d0[x * S + k] = src[x];
        for (int k = 1; k < S; k++) memcpy(d0 + (size_t)k * stride, d0, (size_t)W * S * sizeof *d0);
    }
    s_fallbacks++;
    return 1;
}

/* Camera and sprites at phase p in 1/S pixel units. The scratch copy gets the
 * whole-pixel part; q/r (camera) and each group's fx/fy are the remainders. */
static void wv_apply_motion_q(GVDP *w, double p, int S, int *q, int *r,
                              uint8_t grp[WV_MAX_SPR], WVGroup *g, int *ng)
{
    int dhs = wrap10(s_cam_hs[1] - s_cam_hs[0]), dvs = wrap10(s_cam_vs[1] - s_cam_vs[0]);
    double o  = (dhs > 32 || dhs < -32) ? 0.0 : dhs * (p - 1.0);   /* cut, not a pan */
    double ov = (dvs > 32 || dvs < -32) ? 0.0 : dvs * (p - 1.0);
    int oq = qround(S * o), I = fdiv(oq, S);
    int nv = qround(-S * ov), vq = -nv, J = fdiv(vq, S);
    *q = oq - I * S; *r = vq - J * S;
    if (I) {
        uint32_t base = hs_base(w); int mode = w->reg[11] & 3, step = mode == 3 ? 4 : mode == 2 ? 32 : 0;
        int lines = step ? (mode == 3 ? 224 : 28) : 1;
        for (int i = 0; i < lines; i++) for (int plane = 0; plane < 2; plane++) {
            uint32_t a = base + (uint32_t)i * (uint32_t)step + (uint32_t)plane * 2u;
            uint16_t d = rd16(w, a);
            wr16(w, a, (uint16_t)((d & 0xFC00) | ((d + I) & 0x3FF)));
        }
    }
    if (J) {
        int cols = (w->reg[11] & 4) ? GVDP_VSRAM_ENTRIES : 2;
        for (int i = 0; i < cols; i++) w->vsram[i] = (uint16_t)((w->vsram[i] & 0xFC00) | ((w->vsram[i] + J) & 0x3FF));
    }

    memset(grp, 0, WV_MAX_SPR);
    *ng = 1; g[0].fx = 0; g[0].fy = 0; g[0].rank = INT_MAX;   /* group 0: whole pixels */
    uint32_t sat = sat_base(w);
    for (int i = 0; i < WV_MAX_OBJ; i++) {
        WVObj *ob = &s_obj[i];
        if (!ob->used || !ob->seen || ob->slot < 0 || ob->slot >= WV_MAX_SPR) continue;
        uint32_t e = sat + (uint32_t)ob->slot * 8u;
        uint16_t oy = rd16(w, e), ox = rd16(w, e + 6);
        if ((ox & 0x1FF) == 0) continue;        /* x = 0 is a line mask, not a picture */
        double x, y; obj_display(ob, p, &x, &y);
        /* screen = world + displayed camera; same rounding as the planes, so
         * anything fixed in the world lands on exactly the plane's fraction. */
        int xq = qround(S * (x + o)) + S * s_cam_hs[1];
        int yq = qround(S * (y - ov)) - S * s_cam_vs[1];
        int xs = fdiv(xq, S), fx = xq - xs * S, ys = fdiv(yq, S), fy = yq - ys * S;
        int gi = -1;
        for (int k = 0; k < *ng; k++) if (g[k].fx == fx && g[k].fy == fy) { gi = k; break; }
        if (gi < 0) {
            if (*ng < WV_MAX_GRP) { gi = (*ng)++; g[gi].fx = fx; g[gi].fy = fy; g[gi].rank = INT_MAX; }
            else { xs = fdiv(xq + S / 2, S); ys = fdiv(yq + S / 2, S); gi = 0; }   /* too many: whole pixel */
        }
        wr16(w, e,     (uint16_t)((oy & 0xFC00) | ((ys + 128) & 0x3FF)));
        wr16(w, e + 6, (uint16_t)((ox & 0xFE00) | ((xs + 128) & 0x1FF)));
        grp[ob->slot] = (uint8_t)gi;
    }
}

/* Capture planes (once) and each group's sprite layer, then compose at S x S. */
static int wv_compose_q(GVDP *w, const WVFrame *f, uint32_t *image, int stride, int H, int S,
                        int q, int r, const uint8_t grp[WV_MAX_SPR], WVGroup *g, int ng)
{
    int W = f->width, SW = W * S;
    WVSprite spr[WV_MAX_SPR];
    uint16_t ysave[WV_MAX_SPR];
    int n = wv_sprites(w, spr);
    uint32_t sat = sat_base(w);
    for (int i = 0; i < n; i++) {
        int gi = grp[spr[i].entry];
        if (i < g[gi].rank) g[gi].rank = i;          /* front-most member: link order */
        ysave[i] = rd16(w, sat + (uint32_t)spr[i].entry * 8u);
    }
    for (int gi = 0; gi < ng; gi++) {
        for (int i = 0; i < n; i++)                  /* only this group's sprites visible */
            wr16(w, sat + (uint32_t)spr[i].entry * 8u,
                 grp[spr[i].entry] == gi ? ysave[i] : (uint16_t)(ysave[i] & 0xFC00));
        for (int y = 0; y < H; y++) {
            if (!gvdp_capture_scanline(w, y, &s_row)) return 0;
            if ((int)s_row.width != W || s_row.start || (int)s_row.count != W || !s_row.display ||
                s_row.private_planes || s_row.diagnostic) return 0;
            if (gi == 0) {
                memcpy(s_pa[y], s_row.plane_a, (size_t)W); memcpy(s_pb[y], s_row.plane_b, (size_t)W);
                s_bd[y] = s_row.backdrop; s_sm[y] = s_row.shadow_mode;
            }
            memcpy(s_gs[gi][y], s_row.sprite, (size_t)W);
            uint8_t *fl = s_gf[gi][y];
            for (int x = 0; x < W; x++)
                fl[x] = (uint8_t)((s_row.opaque[x] != 0) | (s_row.high[x] != 0) << 1 |
                                  (s_row.shadow_op[x] != 0) << 2 | (s_row.hilite_op[x] != 0) << 3);
        }
    }
    for (int i = 0; i < n; i++) wr16(w, sat + (uint32_t)spr[i].entry * 8u, ysave[i]);

    int order[WV_MAX_GRP];                           /* back to front */
    for (int i = 0; i < ng; i++) order[i] = i;
    for (int i = 1; i < ng; i++) for (int j = i; j > 0 && g[order[j - 1]].rank < g[order[j]].rank; j--)
        { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }

    uint32_t pal[GVDP_OUTPUT_PALETTE_SIZE];
    wv_palette(w, f->host, pal);
    int wxr = w->reg[17], wyr = w->reg[18], window_x = (wxr & 31) * 16, window_y = (wyr & 31) * 8;
    int right = (wxr & 128) != 0;

    static uint32_t var[4][WV_W];
    uint8_t a[WV_W], b[WV_W], sp[WV_W], op[WV_W], hi[WV_W], so[WV_W], ho[WV_W], idx[WV_W];
    int key[1 + WV_MAX_GRP], prev[1 + WV_MAX_GRP], have_prev = 0;
    for (int y = 0; y < H; y++) {
        int vert = (wyr & 128) ? y >= window_y : y < window_y;
        for (int k = 0; k < S; k++) {
            uint32_t *dst = image + (size_t)(y * S + k) * stride;
            int ya = y + (k + r >= S); if (ya > H - 1) ya = H - 1;
            key[0] = ya;
            for (int gi = 0; gi < ng; gi++) key[1 + gi] = y - (k < g[gi].fy);
            if (have_prev && !memcmp(key, prev, sizeof(int) * (size_t)(1 + ng)) && k) {
                memcpy(dst, dst - stride, (size_t)SW * sizeof *dst);
                continue;
            }
            memcpy(prev, key, sizeof key); have_prev = 1;
            int vmask[4], nvar = 0;
            for (int kx = 0; kx < S; kx++) {
                int mask = kx < q;
                for (int gi = 0; gi < ng; gi++) mask |= (kx < g[gi].fx) << (1 + gi);
                int v = 0;
                while (v < nvar && vmask[v] != mask) v++;
                if (v == nvar) {
                    vmask[nvar++] = mask;
                    int ps = mask & 1;
                    for (int x = 0; x < W; x++) {
                        int c = x - ps; if (c < 0) c = 0;
                        int win = vert || (right ? x >= window_x : x < window_x);
                        a[x] = win ? s_pa[y][x] : s_pa[ya][c];   /* window never scrolls */
                        b[x] = s_pb[ya][c];
                    }
                    memset(sp, 0, (size_t)W); memset(op, 0, (size_t)W); memset(hi, 0, (size_t)W);
                    memset(so, 0, (size_t)W); memset(ho, 0, (size_t)W);
                    for (int oi = 0; oi < ng; oi++) {
                        int gi = order[oi], ys = key[1 + gi], sh = (mask >> (1 + gi)) & 1;
                        if (ys < 0 || ys >= H) continue;
                        const uint8_t *gs = s_gs[gi][ys], *gf = s_gf[gi][ys];
                        for (int x = sh; x < W; x++) {
                            int c = x - sh, fl = gf[c];
                            if (fl & 1) { sp[x] = gs[c]; op[x] = 1; hi[x] = (uint8_t)((fl >> 1) & 1); }
                            so[x] |= (uint8_t)((fl >> 2) & 1); ho[x] |= (uint8_t)((fl >> 3) & 1);
                        }
                    }
                    gvdp_compose_span(idx, a, b, sp, op, hi, so, ho, W, s_bd[y], s_sm[y]);
                    for (int x = 0; x < W; x++) var[v][x] = pal[idx[x]];
                }
                const uint32_t *src = var[v];
                for (int x = 0; x < W; x++) dst[x * S + kx] = src[x];
            }
        }
    }
    return 1;
}

static int wotes_presentation_scale(void)
{
    if (s_scale < 0) {
        const char *e = getenv("WOTES_VIDEO_SCALE");
        s_scale = e && *e ? atoi(e) : 4;
        if (s_scale < 2 || s_scale > 4) s_scale = 1;
        fprintf(stderr, "[WOTES-VIDEO] in-between pictures drawn at %dx\n", s_scale);
    }
    wv_motion_env();
    return s_motion ? s_scale : 1;       /* self-check mode stays native */
}

static int wotes_interpolate_scaled(double phase, uint32_t *image, int stride,
                                    int width, int height, int S)
{
    if (!s_enabled || !s_frames || s_latest < 0) return 0;
    WVFrame *f = &s_frames[s_latest];
    if (!f->complete || f->width != width || f->width > WV_W || f->height != height) return 0;
    wv_motion_env();
    if (!s_motion) return 0;
    double t0 = wv_now_ms();
    s_pictures++;
    wv_advance(f);

    if (f->serial - s_last_log_serial >= 60) {           /* self-check once a second */
        s_work = f->vdp;
        unsigned long long diff = 0;
        if (!wv_render(&s_work, f->host, s_native, WV_W, height, f->width)) diff = 1;
        else for (int y = 0; y < height; y++) {
            const uint32_t *a = &s_native[(size_t)y * WV_W], *b = &f->real[(size_t)y * WV_W];
            for (int x = 0; x < f->width; x++) diff += a[x] != b[x];
        }
        s_checks++;
        if (diff) s_check_fail++;
        s_last_log_serial = f->serial;
        if (s_log)
            fprintf(stderr, "[WOTES-VIDEO] frame %u self-check: %llu px differ (%llu/%llu differed) cam %+d,%+d "
                    "| %dx pictures: avg %.2f ms max %.2f ms, up to %u sprite groups, %llu fallbacks, "
                    "4x-vs-real %llu px (%llu/%llu differed)\n",
                    f->serial, diff, s_check_fail, s_checks,
                    wrap10(s_cam_hs[1] - s_cam_hs[0]), wrap10(s_cam_vs[1] - s_cam_vs[0]),
                    S, s_t_n ? s_t_sum / s_t_n : 0.0, s_t_max, s_t_groups, s_fallbacks,
                    s_qdiff_last, s_qcheck_fail, s_qchecks);
        s_qcheck_pending = 1;
        s_t_sum = s_t_max = 0; s_t_n = s_t_groups = 0;
        if (diff) return wv_scale_real(f, image, stride, height, S);
    }
    if (gvdp_interlace_double(&f->vdp)) return wv_scale_real(f, image, stride, height, S);

    s_work = f->vdp;
    int q, r, ng;
    uint8_t grp[WV_MAX_SPR];
    WVGroup g[WV_MAX_GRP];
    wv_apply_motion_q(&s_work, phase, S, &q, &r, grp, g, &ng);
    if (!wv_compose_q(&s_work, f, image, stride, height, S, q, r, grp, g, ng))
        return wv_scale_real(f, image, stride, height, S);

    /* At phase 1 the 4x picture must be exactly the real frame, enlarged. */
    if (s_qcheck_pending && phase > 0.999) {
        unsigned long long d = 0;
        for (int y = 0; y < height; y++) for (int x = 0; x < f->width; x++)
            d += image[(size_t)(y * S) * stride + (size_t)x * S] != f->real[(size_t)y * WV_W + x];
        s_qchecks++; if (d) s_qcheck_fail++;
        s_qdiff_last = d; s_qcheck_pending = 0;
    }

    double t = wv_now_ms() - t0;
    s_t_sum += t; s_t_n++; if (t > s_t_max) s_t_max = t;
    if ((unsigned)ng > s_t_groups) s_t_groups = (unsigned)ng;
    return 1;
}

const GameVideo wotes_video = {
    .presentation_configure = wotes_presentation_configure,
    .presentation_capture   = wotes_presentation_capture,
    .interpolate            = wotes_interpolate,
    .presentation_reset     = wotes_presentation_reset,
    .presentation_scale     = wotes_presentation_scale,
    .interpolate_scaled     = wotes_interpolate_scaled,
};

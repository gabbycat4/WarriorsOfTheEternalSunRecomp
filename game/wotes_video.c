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
 * Step 4.4: speed + comfort. Layer captures are cached per real frame (a
 * sprite group is re-captured only when its members' whole-pixel positions
 * change, planes only when the whole-pixel camera changes), and rows without a
 * group's sprites ignore that group's fraction, so 240 Hz pictures stay well
 * inside 4.17 ms. Dither blending (WOTES_DITHER_BLEND=0 = off): 1-px checker /
 * stripe patterns in the planes (mountain shading) are shown as their average
 * colour, as a TV showed them, so scrolling doesn't make them shimmer.
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
#include "png_write.h"

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
    /* Step 4.5: the real frame's layers, row by row (GPU layer path). */
    uint8_t  ra[WV_H][WV_W], rb[WV_H][WV_W];    /* plane A (window included) / plane B bytes */
    uint8_t  rf[WV_H][WV_W];                    /* 1 = a sprite pixel is shown here */
    uint8_t  rbd[WV_H];                         /* backdrop index */
    int      raster_ok;                         /* every row captured, no shadow mode */
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
    fprintf(stderr, "[WOTES-VIDEO] high refresh rate %s (step 4.8: GPU layers, averaged battle camera, map memory; 4x CPU pictures as fallback)\n",
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
        f->raster_ok = 1;
    }
    if (width != f->width || width > WV_W) { f->width = -1; return; }   /* unsupported */
    memcpy(&f->real[(size_t)line * WV_W], image, (size_t)width * sizeof *image);
    if (raster && (int)raster->width == width && !raster->start && (int)raster->count == width &&
        raster->display && !raster->private_planes && !raster->diagnostic && !raster->shadow_mode) {
        memcpy(f->ra[line], raster->plane_a, (size_t)width);
        memcpy(f->rb[line], raster->plane_b, (size_t)width);
        for (int x = 0; x < width; x++) f->rf[line][x] = raster->opaque[x] != 0;
        f->rbd[line] = raster->backdrop;
    } else f->raster_ok = 0;
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
    /* Step 33: party members on the map (move through the world but stay on
     * screen while the camera moves) are shown at the average of their last
     * 4 screen positions: their lumpy diagonal steps (pause, then 2 px, or a
     * side step while tracing the leader's path) become even. */
    int      shx[5], shy[5], nsh;  /* screen positions, latest first */
    int      votes;                /* recent camera-moving frames: +1 walks with the camera, -1 not */
    int      party;
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
        {   /* screen history + party vote (Step 33) */
            int sx = spr[k].x, sy = spr[k].y, dsx = 0, dsy = 0;
            if (o->nsh) { dsx = sx - o->shx[0]; dsy = sy - o->shy[0]; }
            if (!o->nsh || dsx > 16 || dsx < -16 || dsy > 16 || dsy < -16) { o->nsh = 0; o->votes = 0; o->party = 0; }
            for (int h = 4; h > 0; h--) { o->shx[h] = o->shx[h - 1]; o->shy[h] = o->shy[h - 1]; }
            o->shx[0] = sx; o->shy[0] = sy;
            if (o->nsh < 5) o->nsh++;
            int cdh = wrap10(s_cam_hs[1] - s_cam_hs[0]), cdv = wrap10(s_cam_vs[1] - s_cam_vs[0]);
            if ((cdh || cdv) && o->nsh >= 2 && cdh <= 4 && cdh >= -4 && cdv <= 4 && cdv >= -4) {
                int walks = abs(dsx) <= 2 && abs(dsy) <= 2 && (dsx - cdh != 0 || dsy + cdv != 0);  /* world step != 0 */
                o->votes += walks ? 1 : -1;
                if (o->votes > 3) o->votes = 3;
                if (o->votes < -3) o->votes = -3;
                if (o->votes >= 2) o->party = 1;
                if (o->votes <= -2) o->party = 0;
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
static GVDPRasterRow s_row;

/* Step 4.4: layer cache. Within one real frame a sprite group's layer only
 * changes when its members' whole-pixel positions change, and the planes only
 * when the whole-pixel camera (I, J) changes, so most 240 Hz pictures reuse
 * earlier captures instead of re-running the VDP renderer up to 5 times. */
#define WV_SLOTS 8
typedef struct {
    int      valid; unsigned serial, stamp; uint64_t key;
    uint8_t  s[WV_H][WV_W];        /* sprite colour */
    uint8_t  f[WV_H][WV_W];        /* 1 opaque, 2 high, 4 shadow op, 8 highlight op */
    uint8_t  any[WV_H];            /* row has any sprite flag */
} WVSprSlot;
typedef struct {
    int      valid, I, J; unsigned serial, stamp;
    uint8_t  a[WV_H][WV_W], b[WV_H][WV_W], bd[WV_H], sm[WV_H];
    uint8_t  da[WV_H][WV_W], db[WV_H][WV_W];   /* 1-px dither pattern marks */
} WVPlaneSlot;
static WVSprSlot   s_sslot[WV_SLOTS];
static WVPlaneSlot s_pslot[2];
static unsigned    s_stamp;
static unsigned long long s_cap_n, s_cap_reuse;
static int         s_dither = -1;              /* WOTES_DITHER_BLEND, default on */
static unsigned long long s_dither_px;
static uint32_t    s_unbl[WV_H][WV_W];          /* sub-pixel (0,0) before blending: exactness check */
static uint8_t     s_mtmp[WV_H][WV_W];

static uint64_t fnv32(uint64_t h, uint32_t v)
{ for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 1099511628211ull; } return h; }

/* Mark pixels inside horizontal ABAB.. runs (4+ pixels). */
static void wv_alt8(const uint8_t *r, int W, uint8_t *m)
{
    memset(m, 0, (size_t)W);
    for (int x = 0; x + 3 < W;) {
        if (r[x] != r[x + 1] && r[x] == r[x + 2] && r[x + 1] == r[x + 3]) {
            int e = x + 3;
            while (e + 1 < W && r[e + 1] == r[e - 1]) e++;
            memset(m + x, 1, (size_t)(e - x + 1));
            x = e + 1;
        } else x++;
    }
}

/* Dither = an alternating run that continues on a neighbouring row (checker
 * or 1-px stripes): the mountain shading. Single-row patterns (text) are not. */
static void wv_mark_dither(uint8_t (*p)[WV_W], uint8_t (*d)[WV_W], int W, int H)
{
    for (int y = 0; y < H; y++) wv_alt8(p[y], W, s_mtmp[y]);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        d[y][x] = (uint8_t)(s_mtmp[y][x] && ((y > 0 && s_mtmp[y - 1][x]) || (y + 1 < H && s_mtmp[y + 1][x])));
}

/* Replace each marked pixel of an ABAB colour run with the A/B average, the way
 * a TV's blur showed it. Static and moving pictures get the same flat shade, so
 * scrolling no longer makes the pattern shimmer. */
static void wv_blend_row(uint32_t *r, const uint8_t *m, int W, int skip_clear)
{
    for (int x = 0; x + 3 < W;) {
        if (r[x] != r[x + 1] && r[x] == r[x + 2] && r[x + 1] == r[x + 3] &&
            (!skip_clear || (r[x] && r[x + 1]))) {
            int e = x + 3;
            while (e + 1 < W && r[e + 1] == r[e - 1]) e++;
            uint32_t A = r[x], B = r[x + 1];
            uint32_t avg = ((A & 0xFEFEFEFEu) >> 1) + ((B & 0xFEFEFEFEu) >> 1) + (A & B & 0x01010101u);
            for (int i = x; i <= e; i++) if (m[i]) { r[i] = avg; s_dither_px++; }
            x = e + 1;
        } else x++;
    }
}
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
static void wv_apply_motion_q(GVDP *w, double p, int S, int *q, int *r, int *pI, int *pJ,
                              uint8_t grp[WV_MAX_SPR], WVGroup *g, int *ng)
{
    int dhs = wrap10(s_cam_hs[1] - s_cam_hs[0]), dvs = wrap10(s_cam_vs[1] - s_cam_vs[0]);
    double o  = (dhs > 32 || dhs < -32) ? 0.0 : dhs * (p - 1.0);   /* cut, not a pan */
    double ov = (dvs > 32 || dvs < -32) ? 0.0 : dvs * (p - 1.0);
    int oq = qround(S * o), I = fdiv(oq, S);
    int nv = qround(-S * ov), vq = -nv, J = fdiv(vq, S);
    *q = oq - I * S; *r = vq - J * S; *pI = I; *pJ = J;
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

/* Slot lookups (per real frame; slots from older frames are free). */
static WVPlaneSlot *wv_plane_find(unsigned serial, int I, int J)
{
    for (int i = 0; i < 2; i++)
        if (s_pslot[i].valid && s_pslot[i].serial == serial && s_pslot[i].I == I && s_pslot[i].J == J)
            return &s_pslot[i];
    return NULL;
}
static WVPlaneSlot *wv_plane_victim(unsigned serial)
{
    for (int i = 0; i < 2; i++) if (!s_pslot[i].valid || s_pslot[i].serial != serial) return &s_pslot[i];
    return s_pslot[0].stamp <= s_pslot[1].stamp ? &s_pslot[0] : &s_pslot[1];
}
static WVSprSlot *wv_spr_find(unsigned serial, uint64_t key)
{
    for (int i = 0; i < WV_SLOTS; i++)
        if (s_sslot[i].valid && s_sslot[i].serial == serial && s_sslot[i].key == key) return &s_sslot[i];
    return NULL;
}
static WVSprSlot *wv_spr_victim(unsigned serial, unsigned now)
{
    WVSprSlot *best = NULL;
    for (int i = 0; i < WV_SLOTS; i++) {
        WVSprSlot *s = &s_sslot[i];
        if (s->stamp == now) continue;                 /* used by this picture */
        if (!s->valid || s->serial != serial) return s;
        if (!best || s->stamp < best->stamp) best = s;
    }
    return best;
}

/* Planes and each group's sprite layer (cached per frame), then compose at S x S. */
static int wv_compose_q(GVDP *w, const WVFrame *f, uint32_t *image, int stride, int H, int S,
                        int q, int r, int I, int J, const uint8_t grp[WV_MAX_SPR], WVGroup *g, int ng)
{
    int W = f->width, SW = W * S;
    WVSprite spr[WV_MAX_SPR];
    uint16_t ysave[WV_MAX_SPR], xw[WV_MAX_SPR];
    int n = wv_sprites(w, spr);
    uint32_t sat = sat_base(w);
    for (int i = 0; i < n; i++) {
        int gi = grp[spr[i].entry];
        if (i < g[gi].rank) g[gi].rank = i;          /* front-most member: link order */
        ysave[i] = rd16(w, sat + (uint32_t)spr[i].entry * 8u);
        xw[i]    = rd16(w, sat + (uint32_t)spr[i].entry * 8u + 6u);
    }

    unsigned now = ++s_stamp;
    WVSprSlot *gsl[WV_MAX_GRP];
    int need[WV_MAX_GRP];
    for (int gi = 0; gi < ng; gi++) {
        uint64_t key = fnv32(1469598103934665603ull, f->serial);
        for (int i = 0; i < n; i++) if (grp[spr[i].entry] == gi) {
            key = fnv32(key, (uint32_t)spr[i].entry);
            key = fnv32(key, (uint32_t)ysave[i] << 16 | xw[i]);
        }
        WVSprSlot *s = wv_spr_find(f->serial, key);
        need[gi] = !s;
        if (!s) { s = wv_spr_victim(f->serial, now); s->valid = 0; s->key = key; }
        else s_cap_reuse++;
        s->stamp = now;
        gsl[gi] = s;
    }
    WVPlaneSlot *pl = wv_plane_find(f->serial, I, J);
    int fill_pl = !pl;
    if (pl) s_cap_reuse++;
    else { pl = wv_plane_victim(f->serial); pl->valid = 0; }
    pl->stamp = now;

    /* gi == ng: planes still missing although every sprite layer was cached. */
    for (int gi = 0; gi <= ng; gi++) {
        WVSprSlot *ss = NULL;
        if (gi < ng) { if (!need[gi]) continue; ss = gsl[gi]; }
        else if (!fill_pl) break;
        for (int i = 0; i < n; i++)                  /* only this group's sprites visible */
            wr16(w, sat + (uint32_t)spr[i].entry * 8u,
                 grp[spr[i].entry] == gi ? ysave[i] : (uint16_t)(ysave[i] & 0xFC00));
        for (int y = 0; y < H; y++) {
            if (!gvdp_capture_scanline(w, y, &s_row)) goto fail;
            if ((int)s_row.width != W || s_row.start || (int)s_row.count != W || !s_row.display ||
                s_row.private_planes || s_row.diagnostic) goto fail;
            if (fill_pl) {
                memcpy(pl->a[y], s_row.plane_a, (size_t)W); memcpy(pl->b[y], s_row.plane_b, (size_t)W);
                pl->bd[y] = s_row.backdrop; pl->sm[y] = s_row.shadow_mode;
            }
            if (ss) {
                memcpy(ss->s[y], s_row.sprite, (size_t)W);
                uint8_t *fl = ss->f[y], any = 0;
                for (int x = 0; x < W; x++) {
                    fl[x] = (uint8_t)((s_row.opaque[x] != 0) | (s_row.high[x] != 0) << 1 |
                                      (s_row.shadow_op[x] != 0) << 2 | (s_row.hilite_op[x] != 0) << 3);
                    any |= fl[x];
                }
                ss->any[y] = any != 0;
            }
        }
        if (fill_pl) {
            wv_mark_dither(pl->a, pl->da, W, H);
            wv_mark_dither(pl->b, pl->db, W, H);
            pl->serial = f->serial; pl->I = I; pl->J = J; pl->valid = 1;
            fill_pl = 0;
        }
        if (ss) { ss->serial = f->serial; ss->valid = 1; }
        s_cap_n++;
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
    uint8_t a[WV_W], b[WV_W], sp[WV_W], op[WV_W], hi[WV_W], so[WV_W], ho[WV_W], idx[WV_W], dm[WV_W];
    int key[1 + WV_MAX_GRP], prev[1 + WV_MAX_GRP], have_prev = 0;
    for (int y = 0; y < H; y++) {
        int vert = (wyr & 128) ? y >= window_y : y < window_y;
        for (int k = 0; k < S; k++) {
            uint32_t *dst = image + (size_t)(y * S + k) * stride;
            int ya = y + (k + r >= S); if (ya > H - 1) ya = H - 1;
            key[0] = ya;
            for (int gi = 0; gi < ng; gi++) {         /* -1: no sprite of this group on that row */
                int ys = y - (k < g[gi].fy);
                key[1 + gi] = (ys >= 0 && ys < H && gsl[gi]->any[ys]) ? ys : -1;
            }
            if (have_prev && k && !memcmp(key, prev, sizeof(int) * (size_t)(1 + ng))) {
                memcpy(dst, dst - stride, (size_t)SW * sizeof *dst);
                continue;
            }
            memcpy(prev, key, sizeof key); have_prev = 1;
            int vmask[4], nvar = 0;
            for (int kx = 0; kx < S; kx++) {
                int mask = kx < q;
                for (int gi = 0; gi < ng; gi++)
                    if (key[1 + gi] >= 0) mask |= (kx < g[gi].fx) << (1 + gi);
                int v = 0;
                while (v < nvar && vmask[v] != mask) v++;
                if (v == nvar) {
                    vmask[nvar++] = mask;
                    int ps = mask & 1, anyd = 0;
                    for (int x = 0; x < W; x++) {
                        int c = x - ps; if (c < 0) c = 0;
                        int win = vert || (right ? x >= window_x : x < window_x);
                        a[x] = win ? pl->a[y][x] : pl->a[ya][c];   /* window never scrolls */
                        b[x] = pl->b[ya][c];
                        dm[x] = (uint8_t)((!win && pl->da[ya][c]) || pl->db[ya][c]);
                        anyd |= dm[x];
                    }
                    memset(sp, 0, (size_t)W); memset(op, 0, (size_t)W); memset(hi, 0, (size_t)W);
                    memset(so, 0, (size_t)W); memset(ho, 0, (size_t)W);
                    for (int oi = 0; oi < ng; oi++) {
                        int gi = order[oi], ys = key[1 + gi], sh = (mask >> (1 + gi)) & 1;
                        if (ys < 0) continue;
                        const uint8_t *gs = gsl[gi]->s[ys], *gf = gsl[gi]->f[ys];
                        for (int x = sh; x < W; x++) {
                            int c = x - sh, fl = gf[c];
                            if (fl & 1) { sp[x] = gs[c]; op[x] = 1; hi[x] = (uint8_t)((fl >> 1) & 1); }
                            so[x] |= (uint8_t)((fl >> 2) & 1); ho[x] |= (uint8_t)((fl >> 3) & 1);
                        }
                    }
                    gvdp_compose_span(idx, a, b, sp, op, hi, so, ho, W, pl->bd[y], pl->sm[y]);
                    for (int x = 0; x < W; x++) var[v][x] = pal[idx[x]];
                    if (k == 0 && kx == 0) memcpy(s_unbl[y], var[v], (size_t)W * sizeof var[v][0]);
                    if (s_dither && anyd) {
                        for (int x = 0; x < W; x++) if (op[x]) dm[x] = 0;   /* sprites stay crisp */
                        wv_blend_row(var[v], dm, W, 0);
                    }
                }
                const uint32_t *src = var[v];
                for (int x = 0; x < W; x++) dst[x * S + kx] = src[x];
            }
        }
    }
    return 1;
fail:
    for (int i = 0; i < n; i++) wr16(w, sat + (uint32_t)spr[i].entry * 8u, ysave[i]);
    return 0;
}

static void wv_dither_env(void)
{
    if (s_dither < 0) { const char *d = getenv("WOTES_DITHER_BLEND"); s_dither = !(d && *d == '0'); }
}

static int wotes_presentation_scale(void)
{
    wv_dither_env();
    if (s_scale < 0) {
        const char *e = getenv("WOTES_VIDEO_SCALE");
        s_scale = e && *e ? atoi(e) : 4;
        if (s_scale < 2 || s_scale > 4) s_scale = 1;
        fprintf(stderr, "[WOTES-VIDEO] in-between pictures drawn at %dx, dither blending %s\n",
                s_scale, s_dither ? "on" : "off (WOTES_DITHER_BLEND=0)");
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
                    "4x-vs-real %llu px (%llu/%llu differed) | captures %llu, reused %llu, dither px %llu\n",
                    f->serial, diff, s_check_fail, s_checks,
                    wrap10(s_cam_hs[1] - s_cam_hs[0]), wrap10(s_cam_vs[1] - s_cam_vs[0]),
                    S, s_t_n ? s_t_sum / s_t_n : 0.0, s_t_max, s_t_groups, s_fallbacks,
                    s_qdiff_last, s_qcheck_fail, s_qchecks, s_cap_n, s_cap_reuse, s_dither_px);
        s_qcheck_pending = 1;
        s_t_sum = s_t_max = 0; s_t_n = s_t_groups = 0;
        s_cap_n = s_cap_reuse = s_dither_px = 0;
        if (diff) return wv_scale_real(f, image, stride, height, S);
    }
    if (gvdp_interlace_double(&f->vdp)) return wv_scale_real(f, image, stride, height, S);

    s_work = f->vdp;
    int q, r, I, J, ng;
    uint8_t grp[WV_MAX_SPR];
    WVGroup g[WV_MAX_GRP];
    wv_apply_motion_q(&s_work, phase, S, &q, &r, &I, &J, grp, g, &ng);
    if (!wv_compose_q(&s_work, f, image, stride, height, S, q, r, I, J, grp, g, ng))
        return wv_scale_real(f, image, stride, height, S);

    /* At phase 1 the 4x picture (before dither blending) must be exactly the
     * real frame, enlarged. */
    if (s_qcheck_pending && phase > 0.999) {
        unsigned long long d = 0;
        for (int y = 0; y < height; y++) for (int x = 0; x < f->width; x++)
            d += s_unbl[y][x] != f->real[(size_t)y * WV_W + x];
        s_qchecks++; if (d) s_qcheck_fail++;
        s_qdiff_last = d; s_qcheck_pending = 0;
    }

    double t = wv_now_ms() - t0;
    s_t_sum += t; s_t_n++; if (t > s_t_max) s_t_max = t;
    if ((unsigned)ng > s_t_groups) s_t_groups = (unsigned)ng;
    return 1;
}

/* ------------------------------------------------------------ GPU layers --
 * Step 4.5 (DuckTales lesson 9): build each real frame ONCE as layers at
 * native size and let the graphics card place them at fractional positions for
 * every in-between picture (GameVideo.presentation_layers). Back to front:
 *   L0  planes without sprites (backdrop, B, A), scrolled, margin LM px
 *   W0  window plane (side panel) composed the same way, never moves
 *   low-priority sprites, one layer per group of sprites that move together
 *   L2  high-priority plane pixels (scrolled), W2 the window's
 *   high-priority sprites, per group
 * That is the VDP's own order (backdrop < low planes < low sprites < high
 * planes < high sprites) because this game never uses shadow/highlight; any
 * frame that does (or interlace, blank rows, ...) falls back to the CPU path.
 * Planes come from the real frame's rows (window on: W0/W2) plus one capture
 * with the window off (the map under the side panel) and one at the previous
 * camera (the edge strip a moving camera reveals). Sprites are decoded from
 * the SAT into their group's layer, keeping only pixels where the real frame
 * shows a sprite (so masks and line limits stay exact), back sprites kept under
 * front ones (no holes when they separate). Once a second the layers are
 * composed at the frame's own positions on the CPU and must equal the real
 * picture, else that frame uses the CPU path. WOTES_VIDEO_GPU=0 = off. */
#define LM 64                          /* margin: room for the eased camera to trail */
#define LCW (WV_W + 2 * LM)
#define LCH (WV_H + 2 * LM)
#define WV_LGRP 14

typedef struct { uint32_t px[WV_H * WV_W]; int x0, y0, x1, y1; } WVSLayer;   /* bbox: [x0,x1) x [y0,y1) */

/* Step 35: map memory for the trailing camera. The VDP plane is only 256 px
 * tall (224 shown): when the camera moves, the game reuses the rows that just
 * left the screen for the rows coming in, so a view trailing the game camera
 * by up to ~55 px saw rewritten rows (Step 34 video: a wrong strip at the top).
 * Every frame the shown map pixels are kept here by WORLD position; the margin
 * the trailing view reveals comes from here (it only trails toward places shown
 * a moment ago), falling back to a capture at the previous camera. */
#define WC 1024                                   /* world cache, wraps every 1024 px */
#define WCB 8                                     /* stamp block size */
static uint32_t s_wc0[WC * WC], s_wc2[WC * WC];   /* L0 / L2 colours by world position */
static unsigned s_wcs[(WC / WCB) * (WC / WCB)];   /* frame stamp per 8x8 block (0 = never) */
static unsigned s_wc_epoch_serial;                /* stamps older than this are invalid */
static unsigned long long s_wc_hits;
#define WC_MAX_AGE 120u                           /* frames */
static int      s_wc_reset_pending = 1;           /* cut / missed frames: forget the memory */

static int s_gpu = -1;
static unsigned s_lay_serial;
static int      s_lay_ok;
static uint32_t s_L0[LCH * LCW], s_L2[LCH * LCW];
static uint32_t s_W0[WV_H * WV_W], s_W2[WV_H * WV_W];
static int      s_have_win, s_cx0, s_cx1, s_cy0, s_cy1;   /* non-window (scrolling) rectangle */
static WVSLayer s_sl[WV_LGRP][2];
static int      s_lgn, s_lg_rep[WV_LGRP], s_lg_rank[WV_LGRP], s_lg_order[WV_LGRP], s_lg_kind[WV_LGRP];
static uint8_t  s_c0a[WV_H][WV_W], s_c0b[WV_H][WV_W], s_c1a[WV_H][WV_W], s_c1b[WV_H][WV_W];
static uint8_t  s_lda[WV_H][WV_W], s_ldb[WV_H][WV_W];
static double   s_lt_sum, s_lt_max;
static unsigned s_lt_n, s_lpics, s_lmax, s_llog_serial;
static unsigned long long s_lchecks, s_lcheck_fail, s_lcheck_last, s_lfall, s_lfall_check;

/* Step 32: eased battle camera (DuckTales "steady camera"). In battles the
 * game's camera pans at a constant 4-5 px per frame that starts and stops
 * instantly (and alternates 3/4-px steps), and it follows the active
 * character in 8-px lurches (8 px in 4 frames, then still). The shown camera
 * follows the game's camera with a critically damped spring instead: gentle
 * start and stop, lurches merged into one glide, never overshooting. It trails
 * the game camera by at most LM-4 px; everything is drawn in world positions,
 * so characters stay exactly where they belong. World map (its main loop runs
 * every frame): the shown camera is the game's camera, exactly as before.
 * WOTES_CAM_EASE=0 off, or a stiffness (default 0.80 per frame).
 * Step 34: a spring alone still pulsed with the 10-12 frame lurch rhythm
 * (shown speed 0.3..1.3 px/frame, Step 33 user: "jerky once it moves"). The
 * spring now follows the game camera averaged over 10 frames and then over 12
 * frames (exact integrals of the frame-to-frame camera): any stop-go rhythm of
 * 10 or 12 frames becomes one steady speed; big swings trail up to ~55 px. */
extern unsigned long long g_wotes_loop_total;     /* wotes_motion.c */
static double   s_ease_w = -1.0;
static int      s_e_have;
static unsigned s_e_serial, s_noloop;
static unsigned long long s_loop_seen;
static double   s_Hp, s_Hl, s_Vp, s_Vl;            /* game camera, unwrapped: previous / latest frame */
static double   s_eH, s_eV, s_evH, s_evV, s_eT;    /* shown camera, velocity (px per frame), time (frames) */
static double   s_e_maxlag;
#define EH 64                                    /* camera history (frames) for the averages */
static double   s_hH[EH], s_hV[EH];               /* game camera of frame k at [k % EH] */

static int wv_easing(void) { return s_ease_w > 0.0 && s_noloop >= 30; }

static void wv_ease_frame(const WVFrame *f)
{
    if (s_ease_w < 0.0) {
        const char *e = getenv("WOTES_CAM_EASE");
        s_ease_w = e && *e ? atof(e) : 0.80;
        if (s_ease_w < 0.0 || s_ease_w > 2.0) s_ease_w = 0.80;
        fprintf(stderr, "[WOTES-VIDEO] battle camera easing %s (%.2f)\n", s_ease_w > 0.0 ? "on" : "off", s_ease_w);
    }
    if (g_wotes_loop_total != s_loop_seen) { s_loop_seen = g_wotes_loop_total; s_noloop = 0; }
    else if (s_noloop < 1000000u) s_noloop++;
    int dh = wrap10(s_cam_hs[1] - s_cam_hs[0]), dv = wrap10(s_cam_vs[1] - s_cam_vs[0]);
    if (!s_e_have || f->serial != s_e_serial + 1) {
        s_Hl = s_Hp = s_eH = s_cam_hs[1]; s_Vl = s_Vp = s_eV = s_cam_vs[1];
        s_evH = s_evV = 0.0; s_eT = (double)f->serial - 1.0; s_e_have = 1;
        for (int k = 0; k < EH; k++) { s_hH[k] = s_Hl; s_hV[k] = s_Vl; }
        s_wc_reset_pending = 1;
    } else {
        s_Hp = s_Hl; s_Vp = s_Vl; s_Hl += dh; s_Vl += dv;
        if (dh > 32 || dh < -32) {                                       /* cut, not a pan */
            s_Hp = s_eH = s_Hl; s_evH = 0.0;
            for (int k = 0; k < EH; k++) s_hH[k] = s_Hl;
            s_wc_reset_pending = 1;
        }
        if (dv > 32 || dv < -32) {
            s_Vp = s_eV = s_Vl; s_evV = 0.0;
            for (int k = 0; k < EH; k++) s_hV[k] = s_Vl;
            s_wc_reset_pending = 1;
        }
    }
    s_hH[f->serial % EH] = s_Hl; s_hV[f->serial % EH] = s_Vl;
    s_hH[(f->serial + EH - 1) % EH] = s_Hp; s_hV[(f->serial + EH - 1) % EH] = s_Vp;
    s_e_serial = f->serial;
}

/* Game camera at time t (frames; frame k's camera at t = k, linear between),
 * and its exact integral from a to b. Times older than the history: oldest. */
static double wv_cam_at(const double *h, double t)
{
    double lo = (double)s_e_serial - (EH - 2), hi = (double)s_e_serial;
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    double n = floor(t), fr = t - n;
    unsigned k = (unsigned)n;
    double a = h[k % EH], b = h[(k + 1) % EH];
    return fr > 0.0 ? a + (b - a) * fr : a;
}
static double wv_cam_int(const double *h, double a, double b)
{
    double sum = 0.0, t = a;
    while (t < b - 1e-12) {
        double e = floor(t) + 1.0; if (e > b) e = b;
        sum += (wv_cam_at(h, t) + wv_cam_at(h, e)) * 0.5 * (e - t);   /* linear piece: exact */
        t = e;
    }
    return sum;
}
/* Average over 10 frames, then over 12 (the second one sampled finely). */
static double wv_cam_avg(const double *h, double t)
{
    const double N1 = 10.0, N2 = 12.0; const int K = 24;
    double acc = 0.0;
    for (int i = 0; i < K; i++) {
        double u = t - N2 + N2 * (i + 0.5) / K;
        acc += wv_cam_int(h, u - N1, u) / N1;
    }
    return acc / K;
}

/* Advance the shown camera to phase p of the latest frame; return its offset
 * from the latest game camera (o: hscroll units, ov: vscroll units). */
static void wv_ease_to(double p, double *o, double *ov)
{
    double T = (double)s_e_serial - 1.0 + p, base = (double)s_e_serial - 1.0;
    if (!wv_easing()) {
        s_eH = s_Hp + (s_Hl - s_Hp) * p; s_eV = s_Vp + (s_Vl - s_Vp) * p;
        s_evH = s_Hl - s_Hp; s_evV = s_Vl - s_Vp;
        if (T > s_eT) s_eT = T;
    } else {
        double w = s_ease_w;
        while (s_eT < T - 1e-9) {
            double dt = T - s_eT; if (dt > 0.25) dt = 0.25;
            s_eT += dt;
            double q = s_eT - base; if (q < 0.0) q = 0.0; if (q > 1.0) q = 1.0;
            double tH = wv_cam_avg(s_hH, base + q), tV = wv_cam_avg(s_hV, base + q);
            s_evH += dt * (w * w * (tH - s_eH) - 2.0 * w * s_evH); s_eH += dt * s_evH;
            s_evV += dt * (w * w * (tV - s_eV) - 2.0 * w * s_evV); s_eV += dt * s_evV;
        }
        double lim = LM - 4;
        if (s_eH - s_Hl >  lim) s_eH = s_Hl + lim;
        if (s_eH - s_Hl < -lim) s_eH = s_Hl - lim;
        if (s_eV - s_Vl >  lim) s_eV = s_Vl + lim;
        if (s_eV - s_Vl < -lim) s_eV = s_Vl - lim;
    }
    *o = s_eH - s_Hl; *ov = s_eV - s_Vl;
    double lag = fabs(*o) > fabs(*ov) ? fabs(*o) : fabs(*ov);
    if (lag > s_e_maxlag) s_e_maxlag = lag;
}

static void wv_shift_scroll(GVDP *w, int I, int J)
{
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
}

/* Planes of frame f with the window off, scroll moved by whole pixels I/J. */
static int wv_cap_planes(const WVFrame *f, int I, int J, uint8_t (*pa)[WV_W], uint8_t (*pb)[WV_W])
{
    GVDP *w = &s_work;
    *w = f->vdp;
    w->reg[17] = 0; w->reg[18] = 0;                       /* no window */
    wv_shift_scroll(w, I, J);
    WVSprite spr[WV_MAX_SPR];
    int n = wv_sprites(w, spr);
    uint32_t sat = sat_base(w);
    for (int i = 0; i < n; i++) {                         /* sprites off: planes only */
        uint32_t e = sat + (uint32_t)spr[i].entry * 8u;
        wr16(w, e, (uint16_t)(rd16(w, e) & 0xFC00));
    }
    for (int y = 0; y < f->height; y++) {
        if (!gvdp_capture_scanline(w, y, &s_row)) return 0;
        if ((int)s_row.width != f->width || s_row.start || (int)s_row.count != f->width || !s_row.display ||
            s_row.private_planes || s_row.diagnostic || s_row.shadow_mode) return 0;
        memcpy(pa[y], s_row.plane_a, (size_t)f->width);
        memcpy(pb[y], s_row.plane_b, (size_t)f->width);
    }
    return 1;
}

/* Fractional screen offset of an object's sprites at phase p, relative to
 * where the latest frame put them. */
static int s_party_smooth = -1;                    /* WOTES_PARTY_SMOOTH=0 off */
static int wv_party_smooth(const WVObj *ob)
{
    if (s_party_smooth < 0) { const char *e = getenv("WOTES_PARTY_SMOOTH"); s_party_smooth = !(e && *e == '0'); }
    return s_party_smooth && ob->party && ob->nsh >= 5 && !wv_easing();
}

static void wv_obj_offset(const WVFrame *f, int oi, double p, double o, double ov, double *dx, double *dy)
{
    const WVObj *ob = &s_obj[oi];
    uint32_t e = sat_base(&f->vdp) + (uint32_t)ob->slot * 8u;
    int sx0 = (rd16(&f->vdp, e + 6) & 0x1FF) - 128, sy0 = (rd16(&f->vdp, e) & 0x3FF) - 128;
    double x, y, X, Y;
    if (wv_party_smooth(ob)) {                          /* screen-space average (Step 33) */
        double ax = 0, ay = 0, bx = 0, by = 0;
        for (int h = 0; h < 4; h++) { ax += ob->shx[h]; ay += ob->shy[h]; bx += ob->shx[h + 1]; by += ob->shy[h + 1]; }
        ax /= 4; ay /= 4; bx /= 4; by /= 4;
        X = bx + (ax - bx) * p - sx0; Y = by + (ay - by) * p - sy0;
    } else {
        obj_display(ob, p, &x, &y);
        X = x + o + s_cam_hs[1] - sx0; Y = y - ov - s_cam_vs[1] - sy0;
    }
    X -= 512.0 * floor((X + 256.0) / 512.0);
    Y -= 512.0 * floor((Y + 256.0) / 512.0);
    if (fabs(X) > 96.0 || fabs(Y) > 96.0) X = Y = 0.0;   /* cut */
    *dx = X; *dy = Y;
}

static void wv_plane_px(uint32_t A, uint32_t B, uint32_t bd, const uint32_t *pal, uint32_t *c0, uint32_t *c2)
{
    unsigned ah = (A & 0xC0u) == 0xC0u, bh = (B & 0xC0u) == 0xC0u;
    unsigned c = ah ? A & 63u : bh ? B & 63u : (A & 64u) ? A & 63u : (B & 64u) ? B & 63u : bd;
    *c0 = pal[c] | 0xFF000000u;
    *c2 = ah ? pal[A & 63u] | 0xFF000000u : bh ? pal[B & 63u] | 0xFF000000u : 0u;
}

static int wv_in_scroll(int x, int y) { return x >= s_cx0 && x < s_cx1 && y >= s_cy0 && y < s_cy1; }

/* The layers composed at the frame's own positions (offset 0) vs the real picture. */
static uint32_t s_lcomp[WV_H * WV_W];
static unsigned long long s_ld_spr, s_ld_plane, s_ld_win;   /* failing pixels: sprite shown / plane only / in window */
static unsigned s_ldumps;
static unsigned long long wv_layers_check(const WVFrame *f)
{
    unsigned long long d = 0;
    for (int y = 0; y < f->height; y++) for (int x = 0; x < f->width; x++) {
        int sc = wv_in_scroll(x, y), i = y * WV_W + x;
        uint32_t c = sc ? s_L0[(y + LM) * LCW + x + LM] : s_W0[i];
        for (int k = 0; k < s_lgn; k++) { uint32_t p = s_sl[s_lg_order[k]][0].px[i]; if (p) c = p; }
        uint32_t h = sc ? s_L2[(y + LM) * LCW + x + LM] : s_W2[i];
        if (h) c = h;
        for (int k = 0; k < s_lgn; k++) { uint32_t p = s_sl[s_lg_order[k]][1].px[i]; if (p) c = p; }
        s_lcomp[i] = c | 0xFF000000u;
        if ((c ^ f->real[(size_t)y * WV_W + x]) & 0xFFFFFFu) {
            d++;
            if (!sc) s_ld_win++;
            else if (f->rf[y][x]) s_ld_spr++;
            else s_ld_plane++;
        }
    }
    if (d && s_ldumps < 4) {                       /* WOTES_LAYER_DUMP=<folder>: PNGs of failing frames */
        const char *dir = getenv("WOTES_LAYER_DUMP");
        if (dir && *dir) {
            char path[1024];
            static uint32_t diffimg[WV_H * WV_W];
            for (int i = 0; i < WV_H * WV_W; i++) diffimg[i] = 0xFF000000u;
            for (int y = 0; y < f->height; y++) for (int x = 0; x < f->width; x++)
                if ((s_lcomp[y * WV_W + x] ^ f->real[(size_t)y * WV_W + x]) & 0xFFFFFFu)
                    diffimg[y * WV_W + x] = 0xFFFF00FFu;
                else diffimg[y * WV_W + x] = 0xFF000000u | ((f->real[(size_t)y * WV_W + x] >> 2) & 0x3F3F3Fu);
            snprintf(path, sizeof path, "%s/layers-fail-%u-real.png", dir, s_ldumps);
            png_write_argb(path, f->real, f->width, f->height, WV_W);
            snprintf(path, sizeof path, "%s/layers-fail-%u-ours.png", dir, s_ldumps);
            png_write_argb(path, s_lcomp, f->width, f->height, WV_W);
            snprintf(path, sizeof path, "%s/layers-fail-%u-diff.png", dir, s_ldumps);
            png_write_argb(path, diffimg, f->width, f->height, WV_W);
            fprintf(stderr, "[WOTES-VIDEO] frame %u layer check: %llu px differ, wrote %s/layers-fail-%u-*.png\n",
                    f->serial, d, dir, s_ldumps);
        }
        s_ldumps++;
    }
    return d;
}

static inline unsigned wv_wc_i(int wx, int wy) { return (unsigned)(wy & (WC - 1)) * WC + (unsigned)(wx & (WC - 1)); }
static inline unsigned wv_wc_b(int wx, int wy) { return (unsigned)((wy & (WC - 1)) / WCB) * (WC / WCB) + (unsigned)((wx & (WC - 1)) / WCB); }
static void wv_wc_put(int wx, int wy, unsigned serial, uint32_t c0, uint32_t c2)
{
    unsigned i = wv_wc_i(wx, wy);
    s_wc0[i] = c0; s_wc2[i] = c2; s_wcs[wv_wc_b(wx, wy)] = serial;
}
static int wv_wc_get(int wx, int wy, unsigned serial, uint32_t *c0, uint32_t *c2)
{
    unsigned st = s_wcs[wv_wc_b(wx, wy)];
    if (!st || st < s_wc_epoch_serial || serial - st > WC_MAX_AGE) return 0;
    unsigned i = wv_wc_i(wx, wy);
    if (!s_wc0[i]) return 0;
    *c0 = s_wc0[i]; *c2 = s_wc2[i];
    return 1;
}

static int wv_build_layers(WVFrame *f, double o_start, double ov_start)
{
    int W = f->width, H = f->height;
    if (!f->raster_ok || W <= 0 || W > WV_W || H <= 0 || H > WV_H || gvdp_interlace_double(&f->vdp)) return 0;
    const GVDP *v = &f->vdp;

    /* The window covers whole rows (vertical part) and/or whole columns
     * (horizontal part); what scrolls is the rectangle left over. */
    int wxr = v->reg[17], wyr = v->reg[18], window_x = (wxr & 31) * 16, window_y = (wyr & 31) * 8;
    s_cx0 = (wxr & 128) ? 0 : window_x; s_cx1 = (wxr & 128) ? window_x : W;
    s_cy0 = (wyr & 128) ? 0 : window_y; s_cy1 = (wyr & 128) ? window_y : H;
    if (s_cx0 > W) s_cx0 = W;
    if (s_cx1 > W) s_cx1 = W;
    if (s_cy0 > H) s_cy0 = H;
    if (s_cy1 > H) s_cy1 = H;
    s_have_win = !(s_cx0 == 0 && s_cx1 == W && s_cy0 == 0 && s_cy1 == H);

    uint8_t (*pa)[WV_W] = f->ra, (*pb)[WV_W] = f->rb;
    if (s_have_win) {
        if (!wv_cap_planes(f, 0, 0, s_c0a, s_c0b)) return 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)   /* outside the window: the real rows */
            if (wv_in_scroll(x, y)) { s_c0a[y][x] = f->ra[y][x]; s_c0b[y][x] = f->rb[y][x]; }
        pa = s_c0a; pb = s_c0b;
    }
    /* The shown camera only moves toward the latest one during this frame, so
     * one capture at its current offset (rounded outward) covers every edge
     * strip the frame's pictures reveal. Map: that is the previous camera. */
    int dhs = wrap10(s_cam_hs[1] - s_cam_hs[0]), dvs = wrap10(s_cam_vs[1] - s_cam_vs[0]);
    int I1 = (dhs > 32 || dhs < -32) ? 0 : -dhs, J1 = (dvs > 32 || dvs < -32) ? 0 : -dvs;
    if (wv_easing()) {
        I1 = (int)(o_start  < 0 ? floor(o_start)  : ceil(o_start));
        J1 = (int)(ov_start < 0 ? floor(ov_start) : ceil(ov_start));
    }
    int Dx = I1, Dy = -J1;              /* latest (x,y) shows at (x+Dx, y+Dy) at the previous camera */
    int have_c1 = (I1 || J1) && wv_cap_planes(f, I1, J1, s_c1a, s_c1b);

    uint32_t pal[GVDP_OUTPUT_PALETTE_SIZE];
    wv_palette(v, f->host, pal);

    int wx0 = (int)s_Hl, wy0 = (int)s_Vl;               /* world = (x - H, y + V) */
    if (s_wc_reset_pending) { s_wc_epoch_serial = f->serial; s_wc_reset_pending = 0; }
    for (int cy = 0; cy < LCH; cy++) {
        int y = cy - LM, yc = y < 0 ? 0 : y >= H ? H - 1 : y;
        uint32_t *r0 = &s_L0[cy * LCW], *r2 = &s_L2[cy * LCW];
        for (int cx = 0; cx < LCW; cx++) {
            int x = cx - LM, xs = x + Dx, ys = y + Dy;
            if (x >= 0 && x < W && y >= 0 && y < H) {
                wv_plane_px(pa[y][x], pb[y][x], f->rbd[y], pal, &r0[cx], &r2[cx]);
                if (!f->rf[y][x] && wv_in_scroll(x, y)) {      /* no sprite: the real pixel is the plane */
                    uint32_t rc = f->real[(size_t)y * WV_W + x] | 0xFF000000u;
                    r0[cx] = rc; if (r2[cx]) r2[cx] = rc;
                }
            }
            else if (wv_wc_get(x - wx0, y + wy0, f->serial, &r0[cx], &r2[cx]))
                s_wc_hits++;
            else if (have_c1 && xs >= 0 && xs < W && ys >= 0 && ys < H)
                wv_plane_px(s_c1a[ys][xs], s_c1b[ys][xs], f->rbd[ys], pal, &r0[cx], &r2[cx]);
            else { r0[cx] = pal[f->rbd[yc]] | 0xFF000000u; r2[cx] = 0; }
        }
    }
    if (s_have_win)
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int i = y * WV_W + x;
            if (wv_in_scroll(x, y)) { s_W0[i] = s_W2[i] = 0; continue; }
            wv_plane_px(f->ra[y][x], f->rb[y][x], f->rbd[y], pal, &s_W0[i], &s_W2[i]);
            if (!f->rf[y][x]) {
                uint32_t rc = f->real[(size_t)y * WV_W + x] | 0xFF000000u;
                s_W0[i] = rc; if (s_W2[i]) s_W2[i] = rc;
            }
        }

    for (int y = s_cy0; y < s_cy1; y++) for (int x = s_cx0; x < s_cx1; x++)
        wv_wc_put(x - wx0, y + wy0, f->serial, s_L0[(y + LM) * LCW + x + LM], s_L2[(y + LM) * LCW + x + LM]);

    /* Sprite groups: sprites whose offsets match at three phases move together. */
    for (int g = 0; g < WV_LGRP; g++) for (int k = 0; k < 2; k++) {
        WVSLayer *L = &s_sl[g][k];
        for (int y = L->y0; y < L->y1; y++) memset(&L->px[y * WV_W + L->x0], 0, (size_t)(L->x1 - L->x0) * 4u);
        L->x0 = WV_W; L->y0 = WV_H; L->x1 = L->y1 = 0;
    }
    WVSprite spr[WV_MAX_SPR];
    int n = wv_sprites(v, spr), sg[WV_MAX_SPR];
    static const double ph[3] = { 0.0, 0.37, 0.71 };
    double sig[WV_LGRP][6];
    s_lgn = 1; s_lg_rep[0] = -1; s_lg_rank[0] = INT_MAX;
    for (int i = 0; i < n; i++) {
        sg[i] = 0;
        int oi = -1;
        int cxm = spr[i].x + spr[i].w / 2, cym = spr[i].y + spr[i].h / 2;
        if (spr[i].x != -128 && wv_in_scroll(cxm, cym))     /* masks and side-panel sprites stay put */
            for (int k = 0; k < WV_MAX_OBJ; k++)
                if (s_obj[k].used && s_obj[k].seen && s_obj[k].slot == spr[i].entry) { oi = k; break; }
        if (oi >= 0) {
            double s6[6];                                   /* own motion only (camera is common) */
            for (int k = 0; k < 3; k++) wv_obj_offset(f, oi, ph[k], 0.0, 0.0, &s6[2 * k], &s6[2 * k + 1]);
            {
                int g = 1;
                int kind = wv_party_smooth(&s_obj[oi]);
                for (; g < s_lgn; g++) {
                    int same = s_lg_kind[g] == kind;
                    for (int k = 0; k < 6; k++) if (fabs(sig[g][k] - s6[k]) > 1e-6) { same = 0; break; }
                    if (same) break;
                }
                if (g == s_lgn && s_lgn < WV_LGRP) {
                    memcpy(sig[g], s6, sizeof s6); s_lg_rep[g] = oi; s_lg_rank[g] = INT_MAX; s_lg_kind[g] = kind; s_lgn++;
                }
                if (g < s_lgn) sg[i] = g;                  /* too many groups: stays put */
            }
        }
        if (i < s_lg_rank[sg[i]]) s_lg_rank[sg[i]] = i;
    }
    for (int g = 0; g < s_lgn; g++) s_lg_order[g] = g;       /* back to front */
    for (int a = 1; a < s_lgn; a++) for (int b = a; b > 0 && s_lg_rank[s_lg_order[b - 1]] < s_lg_rank[s_lg_order[b]]; b--)
        { int t = s_lg_order[b]; s_lg_order[b] = s_lg_order[b - 1]; s_lg_order[b - 1] = t; }

    /* Draw position of each sprite layer: lows (group order), L2, highs. A
     * front sprite's pixel removes back pixels from every layer drawn above its
     * own, so the front sprite shows exactly as the VDP shows it; back pixels in
     * layers below stay (no holes when sprites separate). */
    int lpos[WV_LGRP][2];
    for (int k = 0; k < s_lgn; k++) { lpos[s_lg_order[k]][0] = k; lpos[s_lg_order[k]][1] = WV_LGRP + k; }
    for (int i = n - 1; i >= 0; i--) {                       /* back sprites first */
        const WVSprite *sp = &spr[i];
        if (sp->x == -128) continue;
        int g = sg[i], hp = (sp->attr >> 15) & 1, pb16 = ((sp->attr >> 13) & 3) * 16, ch = sp->h / 8;
        WVSLayer *L = &s_sl[g][hp];
        int mypos = lpos[g][hp];
        for (int py = 0; py < sp->h; py++) {
            int y = sp->y + py;
            if (y < 0 || y >= H) continue;
            int ty = (sp->attr & 0x1000) ? sp->h - 1 - py : py;
            for (int px = 0; px < sp->w; px++) {
                int x = sp->x + px;
                if (x < 0 || x >= W || !f->rf[y][x]) continue;      /* not shown in the real frame */
                int tx = (sp->attr & 0x800) ? sp->w - 1 - px : px;
                uint32_t tile = ((uint32_t)(sp->attr & 0x7FF) + (uint32_t)((tx >> 3) * ch + (ty >> 3))) & 0x7FFu;
                uint8_t byte = v->vram[(tile * 32u + (uint32_t)(ty & 7) * 4u + (uint32_t)((tx & 7) >> 1)) & 0xFFFFu];
                int c = (tx & 1) ? byte & 15 : byte >> 4;
                if (!c) continue;
                int at = y * WV_W + x;
                L->px[at] = pal[pb16 + c] | 0xFF000000u;
                for (int g2 = 0; g2 < s_lgn; g2++) for (int h2 = 0; h2 < 2; h2++)
                    if (lpos[g2][h2] > mypos) s_sl[g2][h2].px[at] = 0;   /* front sprite wins */
                if (x < L->x0) L->x0 = x;
                if (x + 1 > L->x1) L->x1 = x + 1;
                if (y < L->y0) L->y0 = y;
                if (y + 1 > L->y1) L->y1 = y + 1;
            }
        }
    }

    if (f->serial - s_llog_serial >= 60) {                       /* exactness, once a second */
        unsigned long long d = wv_layers_check(f);
        s_lchecks++; s_lcheck_last = d;
        if (d) { s_lcheck_fail++; s_lfall_check++; return 0; }
    }

    if (s_dither) {                                          /* after the check: exact layers first */
        wv_mark_dither(pa, s_lda, W, H);
        wv_mark_dither(pb, s_ldb, W, H);
        uint8_t dm[WV_W];
        for (int y = s_cy0; y < s_cy1; y++) {
            for (int x = 0; x < W; x++)
                dm[x] = (uint8_t)(x >= s_cx0 && x < s_cx1 && (s_lda[y][x] || s_ldb[y][x]));
            wv_blend_row(&s_L0[(y + LM) * LCW + LM], dm, W, 0);
            wv_blend_row(&s_L2[(y + LM) * LCW + LM], dm, W, 1);
        }
    }
    return 1;
}

static void wv_layer(GameVideoLayer *L, unsigned slot, const uint32_t *px, int w, int h, int stride,
                     double x, double y, int clip)
{
    L->pixels = px; L->width = w; L->height = h; L->stride = stride;
    L->slot = slot; L->version = (uint64_t)s_lay_serial << 8 | slot;
    L->x = (float)x; L->y = (float)y;
    if (clip) { L->clip_x = s_cx0; L->clip_y = s_cy0; L->clip_w = s_cx1 - s_cx0; L->clip_h = s_cy1 - s_cy0; }
    else L->clip_x = L->clip_y = L->clip_w = L->clip_h = 0;
}

static int wotes_presentation_layers(double phase, GameVideoLayer *out, int max, int width, int height)
{
    if (!s_enabled || !s_frames || s_latest < 0) return 0;
    if (s_gpu < 0) {
        const char *e = getenv("WOTES_VIDEO_GPU");
        s_gpu = !(e && *e == '0');
        fprintf(stderr, "[WOTES-VIDEO] GPU layers %s\n", s_gpu ? "on" : "off (WOTES_VIDEO_GPU=0)");
    }
    if (!s_gpu) return 0;
    WVFrame *f = &s_frames[s_latest];
    if (!f->complete || f->width != width || f->height != height) return 0;
    wv_motion_env();
    if (!s_motion) return 0;
    wv_dither_env();
    wv_advance(f);

    if (f->serial != s_e_serial || !s_e_have) wv_ease_frame(f);
    double o, ov;
    wv_ease_to(phase, &o, &ov);
    if (f->serial != s_lay_serial) {
        double t0 = wv_now_ms();
        s_lay_serial = f->serial;
        s_lay_ok = wv_build_layers(f, o, ov);
        if (!s_lay_ok) s_lfall++;
        double t = wv_now_ms() - t0;
        s_lt_sum += t; s_lt_n++; if (t > s_lt_max) s_lt_max = t;
        if (f->serial - s_llog_serial >= 60) {
            s_llog_serial = f->serial;
            if (s_log)
                fprintf(stderr, "[WOTES-VIDEO] frame %u GPU layers: build avg %.2f ms max %.2f ms, "
                        "%u pictures, up to %u layers, check %llu px (%llu/%llu failed), "
                        "CPU-path frames %llu, dither px %llu | failing px: sprite %llu, plane %llu, window %llu"
                        " | camera %s, max trail %.1f px, edge px from map memory %llu\n",
                        f->serial, s_lt_n ? s_lt_sum / s_lt_n : 0.0, s_lt_max, s_lpics, s_lmax,
                        s_lcheck_last, s_lcheck_fail, s_lchecks, s_lfall, s_dither_px,
                        s_ld_spr, s_ld_plane, s_ld_win, wv_easing() ? "eased" : "exact", s_e_maxlag, s_wc_hits);
            s_e_maxlag = 0.0; s_wc_hits = 0;
            s_lt_sum = s_lt_max = 0; s_lt_n = s_lpics = s_lmax = 0; s_dither_px = 0;
        }
    }
    if (!s_lay_ok || max < 4 + 2 * WV_LGRP) return 0;

    double gx[WV_LGRP], gy[WV_LGRP];
    gx[0] = gy[0] = 0.0;
    for (int g = 1; g < s_lgn; g++) wv_obj_offset(f, s_lg_rep[g], phase, o, ov, &gx[g], &gy[g]);
    int k = 0, scroll = s_cx1 > s_cx0 && s_cy1 > s_cy0;
    if (scroll) wv_layer(&out[k++], 0, s_L0, LCW, LCH, LCW, -LM + o, -LM - ov, 1);
    if (s_have_win) wv_layer(&out[k++], 2, s_W0, f->width, f->height, WV_W, 0, 0, 0);
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            if (scroll) wv_layer(&out[k++], 1, s_L2, LCW, LCH, LCW, -LM + o, -LM - ov, 1);
            if (s_have_win) wv_layer(&out[k++], 3, s_W2, f->width, f->height, WV_W, 0, 0, 0);
        }
        for (int i = 0; i < s_lgn; i++) {
            int g = s_lg_order[i];
            const WVSLayer *L = &s_sl[g][pass];
            if (L->x1 <= L->x0) continue;
            wv_layer(&out[k++], 4u + 2u * (unsigned)g + (unsigned)pass, &L->px[L->y0 * WV_W + L->x0],
                     L->x1 - L->x0, L->y1 - L->y0, WV_W, L->x0 + gx[g], L->y0 + gy[g], 0);
        }
    }
    s_lpics++;
    if ((unsigned)k > s_lmax) s_lmax = (unsigned)k;
    return k;
}

const GameVideo wotes_video = {
    .presentation_configure = wotes_presentation_configure,
    .presentation_capture   = wotes_presentation_capture,
    .interpolate            = wotes_interpolate,
    .presentation_reset     = wotes_presentation_reset,
    .presentation_scale     = wotes_presentation_scale,
    .interpolate_scaled     = wotes_interpolate_scaled,
    .presentation_layers    = wotes_presentation_layers,
};

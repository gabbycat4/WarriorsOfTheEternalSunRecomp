/*
 * wotes_motion.c: per-frame motion log for Phase 2 (measure before smoothing).
 *
 * Off unless WOTES_MOTION_LOG=<path> is set. One text block per wall frame:
 *   F <frame> it <main-loop iterations this frame> pad <p1>
 *   V <24 VDP regs hex> vs <VSRAM0> <VSRAM1> hs <hscroll A0> <B0> hh <hscroll table hash> ch <CRAM hash>
 *     vh <whole-VRAM hash> (v2)
 *   S <n> <y,size/link,attr,x as 4 hex words per sprite, link order> ...
 *   R <addr>:<bytes> ...   (68000 RAM bytes that changed since the last frame)
 * "it" counts passes through the main loop at $000608 (a game_hook site), so a
 * game that updates every 3rd frame shows it 1,0,0,1,0,0...
 * Read-only: it never writes guest state.
 */
#include "game_spec.h"
#include "genesis_runtime.h"
#include "genesis_machine.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE    *s_log;
static int      s_tried;
static unsigned s_iters;
static uint8_t  s_prev_ram[0x10000];
static int      s_have_prev;

void wotes_motion_main_loop_pass(void) { s_iters++; }

static uint32_t fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint16_t vram16(const GVDP *v, uint32_t a)
{
    a &= 0xFFFE;
    return (uint16_t)((v->vram[a] << 8) | v->vram[a + 1]);
}

void wotes_motion_frame(uint64_t frame)
{
    if (!s_tried) {
        s_tried = 1;
        const char *p = getenv("WOTES_MOTION_LOG");
        if (p && *p) {
            s_log = fopen(p, "w");
            if (s_log) {
                fprintf(s_log, "# WotES motion log v2 (see game/wotes_motion.c for the format)\n");
                fprintf(stderr, "[WOTES-MOTION] logging to %s\n", p);
            } else {
                fprintf(stderr, "[WOTES-MOTION] cannot open %s\n", p);
            }
        }
    }
    if (!s_log) { s_iters = 0; return; }

    const GVDP *v = &g_machine.vdp;
    fprintf(s_log, "F %llu it %u pad %04X\n", (unsigned long long)frame, s_iters,
            (unsigned)g_machine.bus.pad[0]);
    s_iters = 0;

    /* VDP registers, vertical scroll, horizontal scroll table, palette. */
    fprintf(s_log, "V");
    for (int i = 0; i < GVDP_NUM_REGISTERS; i++) fprintf(s_log, " %02X", v->reg[i]);
    uint32_t hs_base = ((uint32_t)v->reg[13] & 0x3Fu) << 10;
    uint16_t hsa = vram16(v, hs_base), hsb = vram16(v, hs_base + 2);
    uint32_t hh = fnv(&v->vram[hs_base & 0xFC00], 0x380);   /* 224 lines x A/B words */
    fprintf(s_log, " vs %04X %04X hs %04X %04X hh %08X ch %08X vh %08X\n",
            v->vsram[0], v->vsram[1], hsa, hsb, hh,
            fnv((const uint8_t *)v->cram, sizeof v->cram),
            fnv(v->vram, GVDP_VRAM_SIZE));   /* whole-VRAM hash: new picture data */

    /* Sprite table in link order (max 80). */
    int h40 = (v->reg[12] & 0x81) != 0;
    uint32_t sat = ((uint32_t)v->reg[5] & (h40 ? 0x7Eu : 0x7Fu)) << 9;
    int max = h40 ? 80 : 64, idx = 0, n = 0;
    char buf[80 * 22 + 16]; int pos = 0;
    for (; n < max; n++) {
        uint32_t e = sat + (uint32_t)idx * 8u;
        uint16_t y = vram16(v, e), sl = vram16(v, e + 2), at = vram16(v, e + 4), x = vram16(v, e + 6);
        pos += snprintf(buf + pos, sizeof buf - pos, " %04X,%04X,%04X,%04X", y, sl, at, x);
        idx = sl & 0x7F;
        if (!idx) { n++; break; }
    }
    fprintf(s_log, "S %d%s\n", n, buf);

    /* Changed RAM bytes, as runs. */
    uint8_t ram[0x10000];
    for (uint32_t i = 0; i < 0x10000; i++) ram[i] = (uint8_t)m68k_read8(0xFF0000u + i);
    fprintf(s_log, "R");
    if (s_have_prev) {
        for (uint32_t i = 0; i < 0x10000; ) {
            if (ram[i] == s_prev_ram[i]) { i++; continue; }
            uint32_t j = i;
            while (j < 0x10000 && (ram[j] != s_prev_ram[j] || (j + 1 < 0x10000 && ram[j + 1] != s_prev_ram[j + 1])))
                j++;
            fprintf(s_log, " %04X:", i);
            for (uint32_t k = i; k < j; k++) fprintf(s_log, "%02X", ram[k]);
            i = j;
        }
    } else {
        fprintf(s_log, " FULL:");
        for (uint32_t i = 0; i < 0x10000; i++) fprintf(s_log, "%02X", ram[i]);
    }
    fputc('\n', s_log);
    memcpy(s_prev_ram, ram, sizeof ram);
    s_have_prev = 1;
    if ((frame & 63) == 0) fflush(s_log);
}

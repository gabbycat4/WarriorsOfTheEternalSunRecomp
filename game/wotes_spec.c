/*
 * wotes_spec.c: GameSpec for Dungeons & Dragons: Warriors of the Eternal Sun
 * (Westwood / Sega, 1992), USA/Europe NTSC.
 *
 * ROM layout: flat 1 MB at $000000-$0FFFFF, identity-mapped.
 * Battery SRAM at $200001-$203FFF (header "RA"): mapped and saved to
 * <rom-name>.srm by the runtime from the header, so sram_start/end stay 0.
 * Boot/dispatch (68K vector table, see game.toml):
 *   EntryPoint   = $000200
 *   VInt (IRQ6)  = $0009E6
 *   HInt         = none (vector points at reset)
 *
 * Minimal bring-up spec modelled on puyo_spec.c / rka_spec.c: the entry and
 * interrupt dispatchers plus ROM identity. Dispatchers go through
 * recomp_call_addr(): the generated dispatch table, Tier-3 floor on a miss.
 */
#include "game_spec.h"
#include "genesis_runtime.h"

#include "m68k_interp.h"
#include "wotes_video.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wotes_call_entry_point(void) { recomp_call_addr(0x000200u); }
static void wotes_call_vblank(void)      { recomp_call_addr(0x0009E6u); }

/* ---- WOTES_INTERP: send chosen routines to the interpreter (debug aid) ----
 * Every function entry is a game_hook site (game.toml [[widescreen_site]]
 * kind = "game_hook"). At runtime, WOTES_INTERP picks which routines run on
 * the clean-room interpreter instead of their generated C:
 *   WOTES_INTERP=all                 every hooked routine
 *   WOTES_INTERP=010000-01FFFF       an address range (hex, inclusive)
 *   WOTES_INTERP=01FFE8,020090-0206FF  a list of single entries / ranges
 * Unset: no effect (one range check per routine entry).
 * A selected routine runs on m68k_interp_run_framed() (the Tier-3 floor's
 * A7-neutral capsule) to its depth-0 return; the hook then reports
 * "handled", exactly like a native RTS. Used to bisect a generated-C
 * divergence: if interpreting a range fixes the symptom, the bad C is in it.
 */
#define WI_MAX 32
static struct { uint32_t lo, hi; } s_wi[WI_MAX];
static int s_wi_n = -1;
static int s_wi_depth;
static unsigned s_wi_fail;

static void wi_parse(void)
{
    s_wi_n = 0;
    const char *e = getenv("WOTES_INTERP");
    if (!e || !*e) return;
    if (strcmp(e, "all") == 0) {
        s_wi[0].lo = 0; s_wi[0].hi = 0xFFFFFFu; s_wi_n = 1;
    } else {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", e);
        for (char *t = strtok(buf, ","); t && s_wi_n < WI_MAX; t = strtok(NULL, ",")) {
            unsigned lo, hi;
            if (sscanf(t, "%x-%x", &lo, &hi) == 2) { }
            else if (sscanf(t, "%x", &lo) == 1) hi = lo;
            else continue;
            s_wi[s_wi_n].lo = lo; s_wi[s_wi_n].hi = hi; s_wi_n++;
        }
    }
    fprintf(stderr, "[WOTES-INTERP] interpreting %d range(s):", s_wi_n);
    for (int i = 0; i < s_wi_n; i++)
        fprintf(stderr, " %06X-%06X", s_wi[i].lo, s_wi[i].hi);
    fprintf(stderr, "\n");
}

static int wi_selected(uint32_t pc)
{
    if (s_wi_n < 0) wi_parse();
    for (int i = 0; i < s_wi_n; i++)
        if (pc >= s_wi[i].lo && pc <= s_wi[i].hi) return 1;
    return 0;
}

/* True when `ret` (the longword on top of the stack) is a return address
 * just after a BSR/JSR in ROM, i.e. this routine was entered by a real call.
 * Pieces of a split routine (entered by fall-through or a branch, with the
 * caller's saved registers on top of the stack) fail this test and keep
 * running natively, so the recompiler's split-stack bookkeeping is never cut
 * in half. They still run on the interpreter whenever the routine they belong
 * to was itself selected. */
static int wi_entered_by_call(uint32_t ret)
{
    if ((ret & 1u) || ret < 8u || ret >= 0x100000u) return 0;
    uint16_t w2 = m68k_read16(ret - 2), w4 = m68k_read16(ret - 4),
             w6 = m68k_read16(ret - 6);
    if ((w2 & 0xFF00u) == 0x6100u && (w2 & 0xFFu) != 0) return 1;   /* BSR.S        */
    if (w4 == 0x6100u) return 1;                                     /* BSR.W        */
    if ((w2 & 0xFFF8u) == 0x4E90u) return 1;                         /* JSR (An)     */
    if ((w4 & 0xFFF8u) == 0x4EA8u || (w4 & 0xFFF8u) == 0x4EB0u ||    /* JSR d(An), (d,An,Xn) */
        w4 == 0x4EB8u || w4 == 0x4EBAu || w4 == 0x4EBBu) return 1;   /* JSR abs.w, d(PC), (d,PC,Xn) */
    if (w6 == 0x4EB9u) return 1;                                     /* JSR abs.l    */
    return 0;
}

void wotes_motion_main_loop_pass(void);   /* wotes_motion.c */
void wotes_motion_frame(uint64_t frame);  /* wotes_motion.c */

static int s_headroom = -1;                 /* CPU headroom (Step 30/31), set on first query */
static unsigned long long s_false_late;     /* Step 33: false "late" flags cleared */

static int wotes_instruction_hook(uint32_t pc)
{
    /* Main-loop pass counter for the motion log (never replaces code). */
    if (pc == 0x000608u) { wotes_motion_main_loop_pass(); return 0; }
    /* Step 33: random battles with CPU headroom. The main loop ($0006E2-$000708)
     * marks a pass "late" ($FFA915 = $FF) if a new frame began, or if the beam
     * is on lines 0-15, or in the active display below line 112. The encounter
     * routine ($012CD8, called at $00070C) CANCELS a pending encounter on a late
     * pass. With headroom the pass ends so early that the beam is often on lines
     * 0-15 of the next screen: a false "late" that ate random battles. Only for
     * that false alarm (no new frame, beam on lines 0-15) the flag is cleared;
     * real overruns keep it. Original timing (headroom 1): untouched. */
    if (pc == 0x012CD8u) {
        if (s_headroom > 1 && m68k_read32(g_cpu.A[7]) == 0x000712u &&
            m68k_read8(0xFFA915u) != 0 && (uint8_t)g_cpu.D[3] == m68k_read8(0xFF8004u) &&
            (m68k_read16(0xC00008u) >> 8) < 0x10u) {
            m68k_write8(0xFFA915u, 0);
            if (++s_false_late <= 3 || s_false_late % 500 == 0)
                fprintf(stderr, "[WOTES] encounter check: false \"late\" pass cleared (%llu so far)\n", s_false_late);
        }
        return 0;
    }
    /* Inside an interpreted routine everything already runs on the
     * interpreter (it calls this hook too): let it continue. */
    if (s_wi_depth || !wi_selected(pc)) return 0;
    uint32_t expected = m68k_read32(g_cpu.A[7]) & 0xFFFFFFu;
    if (!wi_entered_by_call(expected)) {
        static unsigned s_skips;
        if (s_skips++ < 20)
            fprintf(stderr, "[WOTES-INTERP] $%06X entered without a call (top of stack $%06X): "
                    "left native\n", pc, expected);
        return 0;
    }
    uint32_t exit_pc = 0;
    s_wi_depth = 1;
    M68kiStatus st = m68k_interp_run_framed(pc, &exit_pc);
    s_wi_depth = 0;
    if (st != M68KI_OK || exit_pc != expected) {
        if (s_wi_fail++ < 20)
            fprintf(stderr, "[WOTES-INTERP] $%06X: status %d exit $%06X expected $%06X "
                    "(routine already ran; state may be off)\n",
                    pc, (int)st, exit_pc, expected);
    }
    return 1;   /* the routine has run: never run its C as well */
}

/* Step 30: CPU headroom. Now and then a world-map frame needs more than the
 * 7.6 MHz 68000 can do in 1/60 s (measured: ~230-280 RAM bytes change instead
 * of ~50), so the game misses that frame: the map holds still one frame, then
 * catches up with two passes (a "lag frame", in the original too).
 * WOTES_CPU_HEADROOM=N (2..8) lets the main program run N times faster; V-int,
 * DMA, raster and sound keep native timing, so the game still advances once
 * per V-blank, it just finishes in time. Default 4 (Step 31: no lag frames in
 * ~6,800 walking frames; 2x still left ~1/3); WOTES_CPU_HEADROOM=1 = exact
 * original timing. */
static unsigned wotes_main_cpu_divisor(void)
{
    int s = s_headroom;
    if (s < 0) {
        const char *e = getenv("WOTES_CPU_HEADROOM");
        s = e && *e ? atoi(e) : 4;           /* default 4x (Step 31: 0 lag frames) */
        if (s < 1 || s > 8) s = 1;
        fprintf(stderr, "[WOTES] CPU headroom %dx%s\n", s, s > 1 ? "" : " (original timing)");
        s_headroom = s;
    }
    return (unsigned)s;
}

const GameSpec g_game_spec = {
    .video                  = &wotes_video,   /* high refresh rate (Phase 3/4) */
    .display_name           = "Warriors of the Eternal Sun",
    .short_name             = "WOTES",

    /* GoodGEN [!] (UE) dump: CRC32 AF4A9CD7, 1 MB, serial GM MK-1304 -00. */
    .expected_rom_crc32     = 0xAF4A9CD7u,
    .expected_rom_size      = 0x100000u,

    /* No disassembly: heuristic discovery will have holes. Keep the Tier-3
     * interpreter floor on so misses still run correctly and are recorded in
     * floor_coverage.toml / dispatch_misses.toml for the next regen. */
    .tier3_floor_default    = 1,

    /* Text boxes wait for a NEW press, but the party panel ($00EAC8) reads the
     * pad earlier in every (overrunning) frame and would consume each press
     * if new input arrived at line 0. Latch input at the V-int, as BlastEm
     * effectively does, so the text box's read right after the V-int gets it. */
    .pad_latch_at_vint      = 1,

    .call_entry_point       = wotes_call_entry_point,
    .call_vblank            = wotes_call_vblank,
    .call_hblank            = NULL,
    .resume_main_loop_pc    = 0u,
    .save_resume_pc         = NULL,
    .dispatch_main_loop_pc  = 0u,
    .call_periodic          = NULL,

    .on_post_reset          = NULL,
    .on_frame_pre           = NULL,
    .on_frame_post          = wotes_motion_frame,   /* WOTES_MOTION_LOG */
    .on_hblank              = NULL,

    .handle_arg             = NULL,
    .arg_usage              = NULL,
    .dispatch_override      = NULL,
    .instruction_hook       = wotes_instruction_hook,
    .main_cpu_divisor       = wotes_main_cpu_divisor,   /* WOTES_CPU_HEADROOM (Step 30) */

    .fill_frame_record      = NULL,
    .frame_record_version   = 0u,

    .commands               = NULL,
    .command_count          = 0,
};

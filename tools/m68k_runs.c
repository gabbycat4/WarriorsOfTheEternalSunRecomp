/* m68k_runs.c - straight-line decode helper for tools/fold_floor.py.
 * Uses the recompiler's own decoder (m68k-recomp-core), so instruction
 * boundaries match the generated C exactly.
 *   m68k_runs <rom.bin> < addresses (hex, one per line)
 * For each address prints:  ADDR ok|bad  addr:len[:T] ...
 * listing every instruction from ADDR up to and including the first
 * terminator / branch / call (T marks it), or "bad" if one fails to decode.
 * Build (from the project folder):
 *   E=segagenesisrecomp; C=$E/external/m68k-recomp-core
 *   gcc -O1 -std=gnu11 -w -I$E/recompiler/src -I$C/common -I$C/profiles/genesis \
 *       -o tools/m68k_runs tools/m68k_runs.c $E/recompiler/src/rom_parser.c \
 *       $C/common/m68k_decoder.c $C/common/m68k_timing.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rom_parser.h"
#include "m68k_decoder.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: m68k_runs rom.bin < addrs\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t buf[0x400000];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    GenesisRom rom; memset(&rom, 0, sizeof rom);
    rom.rom_data = buf; rom.rom_size = (uint32_t)n;
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        uint32_t a = (uint32_t)strtoul(line, NULL, 16);
        printf("%06X", a);
        int ok = 1; char out[4096]; int pos = 0; out[0] = 0;
        for (int k = 0; k < 200; k++) {
            M68KInstr in;
            if (a >= rom.rom_size || (a & 1) || !m68k_decode(&rom, a, &in) ||
                in.mnemonic == MN_ILLEGAL || in.byte_length == 0) { ok = 0; break; }
            int t = m68k_is_terminator(&in) || m68k_is_call(&in) ||
                    in.mnemonic == MN_Bcc || in.mnemonic == MN_DBcc ||
                    in.mnemonic == MN_BRA || in.mnemonic == MN_JMP;
            pos += snprintf(out + pos, sizeof out - pos, " %06X:%u%s", a,
                            (unsigned)in.byte_length, t ? ":T" : "");
            if (t || pos > 3900) break;
            a += in.byte_length;
        }
        printf(" %s%s\n", ok ? "ok" : "bad", out);
    }
    return 0;
}

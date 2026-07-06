/* m68k.c - runtime for statically-recompiled 68000 Mac code.
 * CPU state, the function table (original address -> lifted C fn), and the
 * dispatch stubs. The Toolbox trap HAL (m68k_trap) lands in Phase 4; here it is
 * a logging stub so lifted code links and runs to its first real trap. */
#include "macrecomp/m68k.h"
#include <stdio.h>
#include <stdlib.h>

M68K M;

/* ---- function table: open-addressing hash of 24-bit code address -> fn ---- */
#define FT_CAP 16384
static struct { uint32_t addr; m68k_fn fn; } ft[FT_CAP];

void m68k_register(uint32_t addr, m68k_fn fn) {
    uint32_t h = (addr * 2654435761u) % FT_CAP;
    for (uint32_t i = 0; i < FT_CAP; i++) {
        uint32_t j = (h + i) % FT_CAP;
        if (ft[j].fn == 0 || ft[j].addr == addr) { ft[j].addr = addr; ft[j].fn = fn; return; }
    }
    fprintf(stderr, "m68k: function table full\n"); exit(1);
}

static m68k_fn ft_lookup(uint32_t addr) {
    uint32_t h = (addr * 2654435761u) % FT_CAP;
    for (uint32_t i = 0; i < FT_CAP; i++) {
        uint32_t j = (h + i) % FT_CAP;
        if (ft[j].fn == 0) return 0;
        if (ft[j].addr == addr) return ft[j].fn;
    }
    return 0;
}

void m68k_call(uint32_t addr) {
    m68k_fn fn = ft_lookup(addr);
    if (!fn) { fprintf(stderr, "m68k_call: no function at %06x\n", addr); return; }
    fn();
}

/* jump through the A5 jump table (jsr d(a5)). The loader fills jt_map from the
 * decrypted jump table (a5 offset -> target code address). Stub until Phase 5. */
static uint32_t jt_map[8192];
void m68k_jt_set(uint32_t a5off, uint32_t addr) { if (a5off < sizeof(jt_map)/4) jt_map[a5off/2] = addr; }
void m68k_jt_call(uint32_t a5off) {
    uint32_t addr = (a5off < sizeof(jt_map)*2) ? jt_map[a5off/2] : 0;
    if (addr) m68k_call(addr);
    else fprintf(stderr, "m68k_jt_call: unmapped A5+%x\n", a5off);
}

void m68k_unimplemented(const char *what, uint32_t addr) {
    fprintf(stderr, "m68k: unimplemented '%s' at %06x\n", what, addr);
}

/* Toolbox trap HAL stub. Phase 4 links a real HAL instead of this object.
 * Kept weak-ish via MACRECOMP_TRAP_STUB so a real HAL can override at link. */
#ifndef MACRECOMP_NO_TRAP_STUB
void m68k_trap(uint16_t word) {
    static int n = 0;
    if (n++ < 20) fprintf(stderr, "m68k_trap: $%04X (no HAL yet)\n", word);
}
#endif

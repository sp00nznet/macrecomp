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

/* low-memory Ticks (0x16A): the system bumps it 60/sec; games busy-wait on it.
 * We advance it on every lifted call so timing loops make progress. */
static void bump_ticks(void){
    uint32_t t=(M.mem[0x16A]<<24)|(M.mem[0x16B]<<16)|(M.mem[0x16C]<<8)|M.mem[0x16D];
    t++; M.mem[0x16A]=t>>24; M.mem[0x16B]=t>>16; M.mem[0x16C]=t>>8; M.mem[0x16D]=t;
}

/* A fake return address pushed by m68k_call. Pascal-convention functions return
 * by popping the caller's return address and `jmp (aX)`-ing to it (also removing
 * their parameters); they hit this sentinel, which unwinds back to C. C-style
 * functions return via RTS and never touch it, so we discard it ourselves. */
#define RET_SENTINEL 0xFFFFFFF0u

volatile uint32_t g_last_call = 0, g_prev_call = 0;  /* watchdog: last two fns entered */
volatile uint32_t g_shadow[512]; volatile int g_shadow_sp = 0;   /* shadow call stack */

void m68k_call(uint32_t addr) {
    if (addr == RET_SENTINEL) return;          /* Pascal fn jmp'd to the fake return */
    m68k_fn fn = ft_lookup(addr);
    if (!fn) { fprintf(stderr, "m68k_call: no function at %06x\n", addr); return; }
    if (addr != g_last_call) { g_prev_call = g_last_call; g_last_call = addr; }
    bump_ticks();
    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr; g_shadow_sp++;
    SP -= 4; m68k_w32(SP, RET_SENTINEL);        /* fake return address on the 68k stack */
    uint32_t after = SP;
    fn();
    if (SP == after) SP += 4;                    /* C-style fn left it; discard */
    /* Pascal fn already popped it (and removed its args); SP is higher — leave it */
    if (g_shadow_sp > 0) g_shadow_sp--;
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

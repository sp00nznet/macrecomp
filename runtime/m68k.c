/* m68k.c - runtime for statically-recompiled 68000 Mac code.
 * CPU state, the function table (original address -> lifted C fn), and the
 * dispatch stubs. The Toolbox trap HAL (m68k_trap) lands in Phase 4; here it is
 * a logging stub so lifted code links and runs to its first real trap. */
#include "macrecomp/m68k.h"
#include <stdio.h>
#include <stdlib.h>

M68K M;

/* ---- function table: open-addressing hash of 24-bit code address -> fn ----
 *
 * The hash answers "is this a function start", which is what a jsr/bsr to a
 * known routine needs and is the common case. It cannot answer "which function
 * contains this address", and the original code reaches computed mid-function
 * addresses through a register. So registration also fills a parallel array of
 * [start,end) ranges, kept sorted and searched only when the hash misses. */
#define FT_CAP 16384
static struct { uint32_t addr; m68k_fn fn; } ft[FT_CAP];

typedef struct { uint32_t start, end; m68k_fn fn; } Range;
static Range rng[FT_CAP];
static int n_rng = 0, rng_sorted = 0;

void m68k_register(uint32_t addr, uint32_t end, m68k_fn fn) {
    uint32_t h = (addr * 2654435761u) % FT_CAP;
    for (uint32_t i = 0; i < FT_CAP; i++) {
        uint32_t j = (h + i) % FT_CAP;
        if (ft[j].fn == 0 || ft[j].addr == addr) { ft[j].addr = addr; ft[j].fn = fn; goto ranged; }
    }
    fprintf(stderr, "m68k: function table full\n"); exit(1);
ranged:
    if (end <= addr) return;                   /* extent unknown: start-only entry */
    if (n_rng >= FT_CAP) { fprintf(stderr, "m68k: range table full\n"); exit(1); }
    rng[n_rng].start = addr; rng[n_rng].end = end; rng[n_rng].fn = fn;
    n_rng++; rng_sorted = 0;
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

static int rng_order(const void *a, const void *b) {
    uint32_t x = ((const Range *)a)->start, y = ((const Range *)b)->start;
    return x < y ? -1 : x > y;
}
static int rng_probe(const void *key, const void *elem) {
    uint32_t a = *(const uint32_t *)key; const Range *r = elem;
    return a < r->start ? -1 : (a >= r->end ? 1 : 0);
}

/* The function whose body covers addr, or 0. Segments load at distinct bases
 * and a segment's functions partition it, so the ranges do not overlap and a
 * binary search over them is well defined. Sorting is deferred to the first
 * interior lookup: registration order is link order, not address order, and a
 * title that never jumps into a function never pays for it. */
static m68k_fn ft_containing(uint32_t addr) {
    if (!n_rng) return 0;
    if (!rng_sorted) { qsort(rng, n_rng, sizeof rng[0], rng_order); rng_sorted = 1; }
    const Range *r = bsearch(&addr, rng, n_rng, sizeof rng[0], rng_probe);
    return r ? r->fn : 0;
}

/* Reported by a lifted prologue handed an address inside its own body that is
 * not an instruction boundary: either data being executed, or a decode that
 * drifted. Distinct from "no function at", which means no owner at all. */
void m68k_entry_miss(uint32_t entry) {
    fprintf(stderr, "m68k: %06x is inside a function but not an instruction boundary\n", entry);
}

/* low-memory Ticks (0x16A): the system bumps it 60/sec; games busy-wait on it.
 * We advance it on every lifted call so timing loops make progress -- and, via
 * m68k_time_slice below, inside loops that make no calls at all. */
static void bump_ticks(void){
    uint32_t t=(M.mem[0x16A]<<24)|(M.mem[0x16B]<<16)|(M.mem[0x16C]<<8)|M.mem[0x16D];
    t++; M.mem[0x16A]=t>>24; M.mem[0x16B]=t>>16; M.mem[0x16C]=t>>8; M.mem[0x16D]=t;
}

/* A fake return address pushed by m68k_call. Pascal-convention functions return
 * by popping the caller's return address and `jmp (aX)`-ing to it (also removing
 * their parameters); they hit this sentinel, which unwinds back to C. C-style
 * functions return via RTS and never touch it, so we discard it ourselves. */
#define RET_SENTINEL 0xCAFE0000u

volatile uint32_t g_last_call = 0, g_prev_call = 0;  /* watchdog: last two fns entered */
volatile uint32_t g_shadow[512]; volatile int g_shadow_sp = 0;   /* shadow call stack */

/* MRMAXCALLS=<n>: stop after n lifted transfers and print the shadow stack.
 *
 * A title that hangs prints nothing, and the hang is often a loop that makes no
 * Toolbox calls at all -- so the trap trace simply stops and says nothing about
 * where. This turns "it hangs" into a call stack.
 *
 * Counts tail jumps as well as calls. A loop that spans functions goes round
 * through m68k_jump (the compiler's shared return tails are reached that way),
 * and counting only calls misses it entirely. A loop wholly inside one lifted
 * function is a plain `goto` and is still invisible here -- for that, the
 * last-call address on the final trap line is the lead.
 *
 * Read once: getenv in the transfer path costs more than the dispatch it
 * guards. */
static long g_calls, g_maxcalls = -1;
static void watchdog(void) {
    fprintf(stderr, "\nm68k: MRMAXCALLS=%ld reached -- shadow stack, innermost last:\n", g_maxcalls);
    int n = g_shadow_sp < 512 ? g_shadow_sp : 512;
    for (int i = 0; i < n; i++) fprintf(stderr, "  [%3d] %06x\n", i, g_shadow[i]);
    if (g_shadow_sp > 512) fprintf(stderr, "  (%d deeper frames not recorded)\n", g_shadow_sp - 512);
    fprintf(stderr, "  last %06x, before %06x\n", g_last_call, g_prev_call);
    exit(2);
}

/* Backward branches decrement this; when it runs out, time advances. The
 * period is a compromise: short enough that a Ticks busy-wait leaves promptly,
 * long enough that an ordinary inner loop is not paying for a call.
 * ponytail: a fixed period, not a real clock. If a title's timing turns out to
 * care about the *rate*, drive this from plat_ticks() instead of a counter. */
#define TICK_PERIOD 2048
int mr_tickdown = TICK_PERIOD;

void m68k_time_slice(void) {
    mr_tickdown = TICK_PERIOD;
    bump_ticks();
}

#ifdef MACRECOMP_LOOPGUARD
/* Backward-branch census. MRMAXLOOPS=<n> stops after n ticks and prints the
 * addresses gone round most, which names the looping instruction directly.
 * ponytail: linear scan over the table; this is a debug build and the hot loop
 * sits in the first few entries within moments. */
static struct { uint32_t pc; long n; } lp[4096];
static int n_lp;
static long g_ticks, g_maxloops = -1;
void m68k_loop_tick(uint32_t pc) {
    if (g_maxloops < 0) { const char *e = getenv("MRMAXLOOPS"); g_maxloops = e ? atol(e) : 0; }
    if (!g_maxloops) return;
    int i;
    for (i = 0; i < n_lp; i++) if (lp[i].pc == pc) { lp[i].n++; break; }
    if (i == n_lp && n_lp < (int)(sizeof lp / sizeof *lp)) { lp[n_lp].pc = pc; lp[n_lp++].n = 1; }
    if (++g_ticks <= g_maxloops) return;

    fprintf(stderr, "\nm68k: MRMAXLOOPS=%ld reached -- hottest backward branches:\n", g_maxloops);
    for (int k = 0; k < 12; k++) {          /* selection sort: 12 of a few thousand */
        int best = -1;
        for (i = 0; i < n_lp; i++) if (lp[i].n >= 0 && (best < 0 || lp[i].n > lp[best].n)) best = i;
        if (best < 0 || lp[best].n <= 0) break;
        fprintf(stderr, "  %10ld x  %06x\n", lp[best].n, lp[best].pc);
        lp[best].n = -1;                     /* mark printed */
    }
    fprintf(stderr, "  last %06x, before %06x, depth %d\n", g_last_call, g_prev_call, g_shadow_sp);
    exit(3);
}
#endif

static int g_watch_a6 = -1;

void m68k_call(uint32_t addr) {
    if (addr == RET_SENTINEL) return;          /* Pascal fn jmp'd to the fake return */
    if (g_watch_a6 < 0) g_watch_a6 = getenv("MRWATCH") != 0;
    if (g_maxcalls < 0) { const char *e = getenv("MRMAXCALLS"); g_maxcalls = e ? atol(e) : 0; }
    if (g_maxcalls > 0 && ++g_calls > g_maxcalls) watchdog();
    uint32_t entry = 0;                        /* 0 = enter at the function's top */
    m68k_fn fn = ft_lookup(addr);
    if (!fn && (fn = ft_containing(addr)) != 0) entry = addr;
    if (!fn) { fprintf(stderr, "m68k_call: no function at %06x (last %06x, before %06x, depth %d)\n",
                       addr, g_last_call, g_prev_call, g_shadow_sp); return; }
    if (addr != g_last_call) { g_prev_call = g_last_call; g_last_call = addr; }
    bump_ticks();
    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr;
    g_shadow_sp++;   /* depth still counts past the buffer it records into */
    SP -= 4; m68k_w32(SP, RET_SENTINEL);        /* fake return address on the 68k stack */
    uint32_t after = SP;
    /* MRWATCH=1: A6 is the frame pointer, and a callee is expected to hand it
     * back unchanged -- link/unlk exist to guarantee exactly that. A lifted
     * function that returns with A6 altered has corrupted its caller's frame,
     * and every local the caller addresses as -n(a6) afterwards is wrong. That
     * is invisible at the call site and disastrous a few frames later, so the
     * check names the callee that did it rather than the victim. */
    uint32_t a6_in = M.a[6];
    fn(entry);
    if (g_watch_a6 && M.a[6] != a6_in)
        fprintf(stderr, "m68k: %06x returned with A6 %06x -> %06x (entry %06x, SP %06x -> %06x)\n",
                addr, a6_in, M.a[6], entry, after, SP);
    if (SP == after) SP += 4;                    /* C-style fn left it; discard */
    /* Pascal fn already popped it (and removed its args); SP is higher — leave it */
    if (g_shadow_sp > 0) g_shadow_sp--;
}

/* rts: the only "return address" this recomp ever pushes is RET_SENTINEL (m68k_call).
 * A compiler return-tail that saved and re-pushed it (movea.l (a7)+,aX; ...; move.l
 * aX,-(a7); rts) leaves the sentinel on top -> pop it so the caller's stack balances.
 * Anything else on top is a stack imbalance we can't follow: leave it and just
 * return to the C caller (m68k_call unwinds), matching the pre-sentinel behavior. */
void m68k_rts(void) { if (m68k_r32(SP) == RET_SENTINEL) SP += 4; }

/* A tail transfer (68k `jmp`): run the target with NO return address pushed, so
 * its eventual rts returns to *our* caller, not to us. This is how the compiler's
 * shared return tails (movea.l (a7)+,aX; ...; rts) and fall-throughs work. */
void m68k_jump(uint32_t addr) {
    if (addr == RET_SENTINEL) return;          /* rts'd to the fake return -> unwind */
    if (g_maxcalls < 0) { const char *e = getenv("MRMAXCALLS"); g_maxcalls = e ? atol(e) : 0; }
    if (g_maxcalls > 0 && ++g_calls > g_maxcalls) watchdog();
    uint32_t entry = 0;
    m68k_fn fn = ft_lookup(addr);
    if (!fn && (fn = ft_containing(addr)) != 0) entry = addr;
    if (!fn) { fprintf(stderr, "m68k_jump: no function at %06x (last %06x, before %06x, depth %d)\n",
                       addr, g_last_call, g_prev_call, g_shadow_sp); return; }
    fn(entry);
}

/* jump through the A5 jump table (jsr d(a5)). The loader fills jt_map from the
 * decrypted jump table (a5 offset -> target code address). Stub until Phase 5. */
/* Indexed by A5 offset in words: entries are 8 bytes apart but code calls
 * entry+2, so word granularity covers both alignments.
 *
 * One capacity, one predicate, used by all three functions. They previously
 * disagreed -- set accepted a5off < 8192 while call accepted a5off < 65536 --
 * so a large jump table had its tail silently dropped on the way in and read
 * back as zero, giving a null dispatch a long way from the cause. An entry that
 * does not fit now says so.
 *
 * ponytail: a flat array. 32K words covers a 64 KB jump table, far past the
 * 8880 bytes HyperCard ships; make it a hash if a title ever exceeds that. */
#define JT_WORDS 32768
static uint32_t jt_map[JT_WORDS];
static int jt_ok(uint32_t a5off) { return a5off / 2 < JT_WORDS; }

void m68k_jt_set(uint32_t a5off, uint32_t addr) {
    if (jt_ok(a5off)) jt_map[a5off / 2] = addr;
    else fprintf(stderr, "m68k_jt_set: A5+%x beyond the jump-table map\n", a5off);
}
void m68k_jt_call(uint32_t a5off) {
    uint32_t addr = jt_ok(a5off) ? jt_map[a5off / 2] : 0;
    if (addr) m68k_call(addr);
    else fprintf(stderr, "m68k_jt_call: unmapped A5+%x\n", a5off);
}
void m68k_jt_jump(uint32_t a5off) {   /* tail jmp through the jump table */
    uint32_t addr = jt_ok(a5off) ? jt_map[a5off / 2] : 0;
    if (addr) m68k_jump(addr);
    else fprintf(stderr, "m68k_jt_jump: unmapped A5+%x\n", a5off);
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

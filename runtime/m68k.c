/* m68k.c - runtime for statically-recompiled 68000 Mac code.
 * CPU state, the function table (original address -> lifted C fn), and the
 * dispatch stubs. The Toolbox trap HAL (m68k_trap) lands in Phase 4; here it is
 * a logging stub so lifted code links and runs to its first real trap. */
#include "macrecomp/m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

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
    /* MRJT=<hex a5 offset>: report what a jump-table call resolves to. Reading
     * a trace of "jsr $1722(a5)" otherwise means resolving the table by hand. */
    {   static uint32_t want = 0xFFFFFFFFu;
        if (want == 0xFFFFFFFFu) { const char *e = getenv("MRJT");
                                   want = e ? (uint32_t)strtoul(e,0,16) : 0; }
        if (want && a5off == want) {
            static int said = 0;
            if (said++ < 200000) fprintf(stderr, "[jt] A5+%x -> %06x (from %06x)\n",
                                    a5off, addr, g_last_call); } }
    if (addr) m68k_call(addr);
    else fprintf(stderr, "m68k_jt_call: unmapped A5+%x\n", a5off);
}
void m68k_jt_jump(uint32_t a5off) {   /* tail jmp through the jump table */
    uint32_t addr = jt_ok(a5off) ? jt_map[a5off / 2] : 0;
    if (addr) m68k_jump(addr);
    else fprintf(stderr, "m68k_jt_jump: unmapped A5+%x\n", a5off);
}


/* An address inside the A5 world's jump table is a routine, not a function
 * start: the loader filled the table, and the entry says where the routine
 * really is. Code normally gets there with `jsr d(a5)`, but a computed call can
 * hand over the absolute address instead, and without this that reads as a call
 * into nowhere. */
static uint32_t via_jump_table(uint32_t addr) {
    uint32_t a5 = M.a[5];
    if (!a5 || addr <= a5) return 0;
    uint32_t off = addr - a5;
    if (!jt_ok(off)) return 0;
    return jt_map[off / 2];
}

uint32_t g_watch_addr = MR_WATCH_OFF;
void mr_watch_hit(uint32_t addr, uint32_t val){
    fprintf(stderr, "[watch] %06x byte %02x, long now %08x  in %06x (from %06x)\n",
            addr, (unsigned)(val & 0xFFu), m68k_r32(addr), g_last_call, g_prev_call);
    /* The registers the store was built from. "Who wrote this" is only half
     * the answer when the value came out of a register loaded elsewhere. */
    if (getenv("MRWATCHREGS")){
        fprintf(stderr, "        a2=%06x a3=%06x a4=%06x a6=%06x sp=%06x  8(a6)=%08x\n",
                M.a[2], M.a[3], M.a[4], M.a[6], SP, m68k_r32(M.a[6]+8));
        for (int i = 0; i < g_shadow_sp && i < 40; i++)
            fprintf(stderr, "        [%d] %06x\n", i, g_shadow[i]); }
}

volatile uint32_t g_last_call = 0, g_prev_call = 0;  /* watchdog: last two fns entered */

/* Reported by a lifted prologue handed an address inside its own body that is
 * not an instruction boundary: either data being executed, or a decode that
 * drifted. Distinct from "no function at", which means no owner at all. */
void m68k_entry_miss(uint32_t entry) {
    fprintf(stderr, "m68k: %06x is inside a function but not an instruction boundary "
                    "(last %06x, before %06x)\n", entry, g_last_call, g_prev_call);
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
volatile uint32_t g_shadow[512]; volatile int g_shadow_sp = 0;   /* shadow call stack */

/* The fake return address carries the call depth that pushed it, so a return
 * can be told from a non-local exit. Compiled Pascal does the latter with
 * `movea.l <saved frame>,a6; lea -n(a6),a7` and then simply carries on: the
 * guest abandons every frame in between. Executed as an ordinary jump, those
 * abandoned frames stay on the C stack, run their epilogues on the way out and
 * pop the guest stack again for each one -- which walks SP down past zero and
 * turns every later read into a 0. Matching the depth lets us unwind the C
 * stack the same way the guest unwound its own. */
#define RET_SENTINEL_BASE 0xCAFE0000u
#define RET_SENTINEL (RET_SENTINEL_BASE | 0xFFFFu)   /* depth-less, for tests */
#define IS_SENTINEL(v) (((v) & 0xFFFF0000u) == RET_SENTINEL_BASE)
#define SENTINEL_DEPTH(v) ((int)((v) & 0xFFFFu))
#define MAX_UNWIND 512
static jmp_buf g_unwind[MAX_UNWIND];

/* Unwind to the frame that pushed this sentinel. Returning to our own frame is
 * an ordinary return and needs no help. */
static void unwind_to(int depth){
    /* MRNOUNWIND=1 falls back to the old behaviour, for bisecting. */
    { static int off = -1;
      if(off < 0) off = getenv("MRNOUNWIND") != 0;
      if(off) return; }
    if(depth < 0 || depth >= MAX_UNWIND) return;
    if(depth >= g_shadow_sp - 1) return;          /* our own frame: normal */
    g_shadow_sp = depth + 1;
    longjmp(g_unwind[depth], 1);
}


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

static uint32_t g_brk = 0xFFFFFFFFu;
/* Shared by the breakpoint and by any trap that wants it: see MRBRKFIND. */
void m68k_find_probe(const char *tag){
          { const char *want = getenv("MRBRKFIND");
          if (want && *want) {
            size_t wl = strlen(want);
            for (uint32_t b = 0; b + wl < M.memsize; b++) {
                if (memcmp(M.mem + b, want, wl) != 0) continue;
                fprintf(stderr, "[%s] found \"%s\" at %06x:", tag, want, b);
                uint32_t lo = b > 512 ? b - 512 : 0, hi = b + 512;
                for (int i = 0; i < 8; i++) {
                    if (M.d[i] >= lo && M.d[i] <= hi) fprintf(stderr, " d%d=%+d", i, (int)(M.d[i]-b));
                    if (M.a[i] >= lo && M.a[i] <= hi) fprintf(stderr, " a%d=%+d", i, (int)(M.a[i]-b)); }
                for (long o = -0x7000; o < 0x1000; o += 2) {
                    uint32_t v = m68k_r32((uint32_t)(M.a[5] + o));
                    if (v >= lo && v <= hi) fprintf(stderr, " a5%+ld=%+d", o, (int)(v-b)); }
                fprintf(stderr, "\n");
                /* And the text itself: a script that was mangled on the way
                 * into memory explains a parse error with no further work. */
                {   uint32_t f = b > 360 ? b - 360 : 0;
                    fprintf(stderr, "[%s] text %06x: ", tag, f);
                    for (uint32_t i = f; i < b + 80 && i < M.memsize; i++){
                        unsigned char c = M.mem[i];
                        fputc(c == 13 ? 10 : (c >= 32 && c < 127 ? c : 46), stderr); }
                    fprintf(stderr, "\n"); }
                break; } } }
}

void m68k_call(uint32_t addr) {
    if (IS_SENTINEL(addr)) { unwind_to(SENTINEL_DEPTH(addr)); return; }
    /* MRODD=1: catch the first moment the stack or frame pointer goes odd.
     * A 68000 stack is always even; once it is not, every later frame is one
     * byte out and the values read back are garbage that looks plausible. */
    { static int on = -1; static int fired;
      if (on < 0) on = getenv("MRODD") != 0;
      /* A2-A4 are callee-saved and in this title hold frame and record
       * pointers, so an odd one is as impossible as an odd SP -- and it is
       * the register, not the stack, that goes bad here. */
      if (on && fired < 6 && ((SP & 1) || (M.a[6] & 1) ||
                           (M.a[2] & 1) || (M.a[3] & 1) || (M.a[4] & 1))) {
          fired++;
          fprintf(stderr, "[odd] entering %06x sp=%06x a2=%06x a3=%06x a4=%06x a6=%06x\n",
                  addr, SP, M.a[2], M.a[3], M.a[4], M.a[6]);
          for (int i = 0; i < g_shadow_sp && i < 40; i++)
              fprintf(stderr, "      [%d] %06x\n", i, g_shadow[i]); } }
    if (g_watch_a6 < 0) g_watch_a6 = getenv("MRWATCH") != 0;
    if (g_maxcalls < 0) { const char *e = getenv("MRMAXCALLS"); g_maxcalls = e ? atol(e) : 0; }
    if (g_maxcalls > 0 && ++g_calls > g_maxcalls) watchdog();
    /* MRBRK=<hex addr>: report the arguments a chosen function is called with.
     * Pascal arguments sit above the return address, so print a few longwords
     * from there along with the registers. */
    if (g_brk == 0xFFFFFFFFu) { const char *e = getenv("MRBRK"); g_brk = e ? strtoul(e,0,16) : 0; }
    if (g_brk && addr == g_brk) {
        fprintf(stderr, "[brk %06x] d0=%08x d1=%08x d2=%08x a0=%06x a1=%06x sp=%06x a3=%06x a4=%06x a6=%06x\n",
                addr, M.d[0], M.d[1], M.d[2], M.a[0], M.a[1], SP,
                M.a[3], M.a[4], M.a[6]);
        /* MRBRKA5=<signed decimal offsets, comma separated>: the A5 globals to
         * show alongside. An assertion that compares two globals says nothing
         * until you can see what they hold. */
        { const char *g = getenv("MRBRKA5");
          if (g) { fprintf(stderr, "[brk %06x] a5=%06x:", addr, M.a[5]);
            while (*g) { long o = strtol(g, (char**)&g, 10);
                uint32_t v = m68k_r32((uint32_t)(M.a[5] + o));
                fprintf(stderr, " [%ld]=%08x", o, v);
                /* A global that holds a handle says nothing on its own -- follow
                 * it and show the first words of the table it leads to. */
                for (int d = 0; d < 2 && v >= 64 && v + 16 < M.memsize; d++) {
                    uint32_t nv = m68k_r32(v);
                    fprintf(stderr, " %s%06x{", d ? "**" : "*", v);
                    for (int w = 0; w < 6; w++) fprintf(stderr, "%s%04x", w?" ":"", m68k_r16(v + 2u*w));
                    fprintf(stderr, "}");
                    v = nv;
                }
                while (*g == ',' || *g == ' ') g++; }
            fprintf(stderr, "\n"); } }
        /* MRBRKMEM=<hex addr>:<n longs>: dump guest memory at the breakpoint.
         * An argument is often a pointer to a record, and the record is what
         * you actually need to see. */
        { const char *m = getenv("MRBRKMEM");
          if (m) { unsigned base=0, n=0;
            if (sscanf(m, "%x:%u", &base, &n) == 2 && n && n <= 32) {
                fprintf(stderr, "[brk %06x] mem %06x:", addr, base);
                for (unsigned i = 0; i < n; i++)
                    fprintf(stderr, " %08x", m68k_r32(base + 4u*i));
                fprintf(stderr, "\n"); } } }
        /* MRBRKFIND=<text>: locate that text in guest memory, then report every
         * register and A5 global pointing into it. A parser that stopped in the
         * wrong place cannot be found from register values alone -- what you
         * need is the cursor's offset within the source it is reading. */
        m68k_find_probe("brk");
        /* MRSTACK: the whole shadow stack at the breakpoint. A bad argument
         * is made somewhere above the frame that passes it on. */
        if (getenv("MRSTACK"))
            for (int i = 0; i < g_shadow_sp && i < 40; i++)
                fprintf(stderr, "      [%d] %06x\n", i, g_shadow[i]);
        fprintf(stderr, "[brk %06x] args:", addr);
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", m68k_r32(SP + 4u*i));
        /* And the same bytes as text. Pascal passes short strings by value,
         * so the argument you want is often *in* the frame, not behind a
         * pointer, and hex hides it. */
        fprintf(stderr, "  |");
        for (uint32_t i = 0; i < 48 && SP + i < M.memsize; i++){
            unsigned char c = M.mem[SP + i];
            fputc(c >= 32 && c < 127 ? c : 46, stderr); }
        fprintf(stderr, "|");
        fprintf(stderr, "\n");
        /* An argument that points at text is usually the interesting one: print
         * it, and one level of indirection too, since a handle looks the same. */
        for (int i = 0; i < 8; i++) {
            uint32_t v = m68k_r32(SP + 4u*i);
            for (int deref = 0; deref < 2; deref++) {
                if (deref) { if (v < 64 || v + 4 >= M.memsize) break; v = m68k_r32(v); }
                if (v < 64 || v + 48 >= M.memsize) continue;
                int ok = 0;
                for (int k = 0; k < 40; k++) { uint8_t c = M.mem[v+k];
                    if ((c >= 0x20 && c < 0x7f) || c == 0x0D || c == 0x09) ok++; }
                if (ok < 16) continue;
                fprintf(stderr, "[brk] arg%d%s -> %06x: \"", i, deref ? "*" : "", v);
                for (int k = 0; k < 48; k++) { uint8_t c = M.mem[v+k];
                    fputc(c == 0x0D ? '|' : (c >= 0x20 && c < 0x7f ? c : '.'), stderr); }
                fprintf(stderr, "\"\n");
            }
        }
        fprintf(stderr, "\n");
    }
    uint32_t entry = 0;                        /* 0 = enter at the function's top */
    m68k_fn fn = ft_lookup(addr);
    if (!fn && (fn = ft_containing(addr)) != 0) {
        entry = addr;
        /* MRMID=1: a call that lands inside a lifted function rather than on
         * its top. The label switch resumes there correctly, but the prologue
         * never ran -- no link, no movem, no argument loads -- so every
         * callee-saved register still holds the *caller's* value and every
         * d(a6) reference belongs to somebody else's frame. */
        static int on = -1; static long n;
        if (on < 0) on = getenv("MRMID") != 0;
        if (on && n++ < 40)
            fprintf(stderr, "[mid] entering %06x inside a function (from %06x) a3=%06x a6=%06x sp=%06x\n",
                    addr, g_last_call, M.a[3], M.a[6], SP);
    }
    if (!fn) { uint32_t t = via_jump_table(addr);
               if (t) { m68k_call(t); return; } }
    if (!fn) { fprintf(stderr, "m68k_call: no function at %06x (last %06x, before %06x, depth %d)\n",
                       addr, g_last_call, g_prev_call, g_shadow_sp); return; }
    /* Saved and restored around the call: without the restore, "last function
     * entered" stays pointing at whatever was called *deepest*, so every trap
     * reported after a call returns names the wrong caller. That sends you
     * reading a function that had nothing to do with it. */
    /* MRSEGS=1: count function entries per 64K segment and print the histogram
     * periodically. Counting trap *callers* instead, as I first did, only sees
     * segments that happen to make Toolbox calls -- a segment can run hard and
     * never appear. */
    { static int on = -1; static long seg[256], n;
      if (on < 0) on = getenv("MRSEGS") != 0;
      if (on) { seg[(addr >> 16) & 0xFF]++;
        if (++n % 50000 == 0) {
            fprintf(stderr, "[segs]");
            for (int i = 0; i < 256; i++) if (seg[i])
                fprintf(stderr, " %d:%ld", i - 0x50, seg[i]);
            fprintf(stderr, "\n"); } } }
    uint32_t last_in = g_last_call, prev_in = g_prev_call;
    if (addr != g_last_call) { g_prev_call = g_last_call; g_last_call = addr; }
    bump_ticks();
    if (g_shadow_sp < 512) g_shadow[g_shadow_sp] = addr;
    g_shadow_sp++;   /* depth still counts past the buffer it records into */
    int mydepth = g_shadow_sp - 1;
    if(mydepth < 0 || mydepth >= MAX_UNWIND) mydepth = MAX_UNWIND - 1;
    SP -= 4; m68k_w32(SP, RET_SENTINEL_BASE | (uint32_t)mydepth);
    volatile uint32_t after = SP;
    volatile int unwound = 0;
    /* MRWATCH=1: A6 is the frame pointer, and a callee is expected to hand it
     * back unchanged -- link/unlk exist to guarantee exactly that. A lifted
     * function that returns with A6 altered has corrupted its caller's frame,
     * and every local the caller addresses as -n(a6) afterwards is wrong. That
     * is invisible at the call site and disastrous a few frames later, so the
     * check names the callee that did it rather than the victim. */
    /* The stack pointer leaving the address space is the first domino: every
     * later read returns 0 and every write is dropped, so the damage shows up
     * as null pointers far from the cause. Report the first time only. */
    if (g_watch_a6 && SP >= M.memsize) {
        static int said = 0;
        if (!said++) fprintf(stderr, "m68k: SP %08x is outside the %u-byte address space "
                                     "on entry to %06x (last %06x, before %06x)\n",
                             SP, M.memsize, addr, g_last_call, g_prev_call);
    }
    volatile uint32_t a6_in = M.a[6], sp_in = SP;
    /* MRWATCH also checks the callee-saved registers. Mac Pascal preserves
     * D3-D7 and A2-A4 across a call; a lifted function that returns with one of
     * them altered has silently corrupted a value its caller is still holding,
     * and the damage shows up later as a wrong index or a stray pointer with
     * nothing to connect it to the callee that did it. */
    uint32_t sav[8];
    if (g_watch_a6) { for (int i = 0; i < 5; i++) sav[i] = M.d[3+i];
                      for (int i = 0; i < 3; i++) sav[5+i] = M.a[2+i]; }
    if(setjmp(g_unwind[mydepth]) == 0) fn(entry);
    else unwound = 1;                 /* a deeper frame exited non-locally */
    /* MRRET=<hex addr>: print what a function hands back. A Pascal callee pops
     * the sentinel and its own arguments, so SP is left pointing at the result
     * slot the caller reserved -- which is the one thing MRBRK, reporting only
     * on entry, can never show. Answering "does this return zero?" by
     * inference rather than by reading it is how several wrong conclusions got
     * made in this repo's history. */
    {   static uint32_t rw = 0xFFFFFFFFu;
        if (rw == 0xFFFFFFFFu) { const char *e = getenv("MRRET");
                                 rw = e ? (uint32_t)strtoul(e,0,16) : 0; }
        if (rw && addr == rw)
            fprintf(stderr, "[ret %06x] = %08x (word %04x, byte %02x) sp=%06x\n",
                    addr, m68k_r32(SP), m68k_r16(SP), m68k_r8(SP), SP); }
    if (g_watch_a6 && SP >= M.memsize && sp_in < M.memsize) {
        static int said2 = 0;
        if (!said2++) fprintf(stderr, "m68k: %06x left SP at %08x (was %08x) -- "
                                      "outside the address space\n", addr, SP, sp_in);
    }
    if (g_watch_a6) {
        static const char *nm[8] = {"d3","d4","d5","d6","d7","a2","a3","a4"};
        for (int i = 0; i < 8; i++) {
            uint32_t now = i < 5 ? M.d[3+i] : M.a[2+(i-5)];
            if (now == sav[i]) continue;
            fprintf(stderr, "m68k: %06x did not preserve %s (%08x -> %08x)\n",
                    addr, nm[i], sav[i], now);
        }
    }
    /* A Pascal callee pops its own arguments, so SP legitimately comes back
     * higher than it went in -- but never above the caller's frame pointer.
     * The caller's saved A6 and return address live there, and a callee that
     * pops past them overwrites its own caller's frame with the next push.
     * That is where a lost A6 starts; the A6 check below only sees it once the
     * damage is already done, several frames later. */
    if (g_watch_a6 && M.a[6] == a6_in && a6_in && a6_in < M.memsize && SP > a6_in) {
        static int nsp = 0;
        if (nsp++ < 12)
            fprintf(stderr, "m68k: %06x popped past its caller's frame: "
                            "SP %06x -> %06x, caller A6 %06x (entry %06x)\n",
                    addr, after, SP, a6_in, entry);
    }
    if (g_watch_a6 && M.a[6] != a6_in)
        fprintf(stderr, "m68k: %06x returned with A6 %06x -> %06x (entry %06x, SP %06x -> %06x)\n",
                addr, a6_in, M.a[6], entry, after, SP);
    /* After a non-local exit the guest chose SP itself; do not second-guess it. */
    if (!unwound && SP == after) SP += 4;        /* C-style fn left it; discard */
    /* Pascal fn already popped it (and removed its args); SP is higher — leave it */
    if (g_shadow_sp > 0) g_shadow_sp--;
    g_last_call = last_in; g_prev_call = prev_in;
}

/* rts: the only "return address" this recomp ever pushes is RET_SENTINEL (m68k_call).
 * A compiler return-tail that saved and re-pushed it (movea.l (a7)+,aX; ...; move.l
 * aX,-(a7); rts) leaves the sentinel on top -> pop it so the caller's stack balances.
 * Anything else on top is a stack imbalance we can't follow: leave it and just
 * return to the C caller (m68k_call unwinds), matching the pre-sentinel behavior. */
void m68k_rts(void) {
    uint32_t v = m68k_r32(SP);
    if (IS_SENTINEL(v)) { SP += 4; unwind_to(SENTINEL_DEPTH(v)); }
}

/* A tail transfer (68k `jmp`): run the target with NO return address pushed, so
 * its eventual rts returns to *our* caller, not to us. This is how the compiler's
 * shared return tails (movea.l (a7)+,aX; ...; rts) and fall-throughs work. */
void m68k_jump(uint32_t addr) {
    if (IS_SENTINEL(addr)) { unwind_to(SENTINEL_DEPTH(addr)); return; }
    if (g_maxcalls < 0) { const char *e = getenv("MRMAXCALLS"); g_maxcalls = e ? atol(e) : 0; }
    if (g_maxcalls > 0 && ++g_calls > g_maxcalls) watchdog();
    uint32_t entry = 0;
    m68k_fn fn = ft_lookup(addr);
    if (!fn && (fn = ft_containing(addr)) != 0) entry = addr;
    if (!fn) { uint32_t t = via_jump_table(addr);
               if (t) { m68k_jump(t); return; } }
    if (!fn) { fprintf(stderr, "m68k_jump: no function at %06x (last %06x, before %06x, depth %d)\n",
                       addr, g_last_call, g_prev_call, g_shadow_sp); return; }
    /* A tail jump is an entry too. Watching only m68k_call hides every
     * function reached this way -- which is how a breakpoint on the writer
     * of a bad value kept showing the wrong arguments. */
    if (g_brk != 0xFFFFFFFFu && g_brk && addr == g_brk)
        fprintf(stderr, "[brk %06x] via JUMP from %06x sp=%06x a3=%06x a4=%06x a6=%06x arg=%08x\n",
                addr, g_last_call, SP, M.a[3], M.a[4], M.a[6], m68k_r32(SP));
    fn(entry);
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

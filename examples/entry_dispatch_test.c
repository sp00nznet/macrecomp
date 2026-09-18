/* entry_dispatch_test.c - check that a jump into the middle of a function lands.
 *
 * The original code reaches computed addresses through a register, and such a
 * target is usually not a branch target anywhere in the binary. A table of
 * function *starts* cannot express that, so m68k_jump used to report
 * "no function at" and drop the transfer on the floor.
 *
 * The two functions below are hand-written in the shape lift68k.py now emits:
 * a prologue switch over the instruction boundaries, then the body. They stand
 * in for generated code so this runs without a recompiled title.
 *
 * No SDL, no HAL, no display. Exits non-zero if any check fails. */
#include "macrecomp/m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEG_BASE 0x500000u
static uint32_t g_seg_base = SEG_BASE;

static uint32_t trace[16];
static int n_trace;
static void hit(uint32_t v){ if (n_trace < 16) trace[n_trace++] = v; }

/* boundaries at +0x00, +0x10, +0x20; body ends at +0x30 */
static void fn_a(uint32_t entry){
    if(entry) switch(entry-g_seg_base){
        case 0x0u:  goto L0;
        case 0x10u: goto L10;
        case 0x20u: goto L20;
        default: m68k_entry_miss(entry); return; }
 L0:  hit(0x00);
 L10: hit(0x10);
 L20: hit(0x20);
    m68k_rts();
}

/* a second function, so the search has to pick the right owner */
static void fn_b(uint32_t entry){
    if(entry) switch(entry-g_seg_base){
        case 0x30u: goto L30;
        case 0x40u: goto L40;
        default: m68k_entry_miss(entry); return; }
 L30: hit(0x30);
 L40: hit(0x40);
    m68k_rts();
}

static int failures;
static void expect(const char *what, const uint32_t *want, int n){
    int ok = (n_trace == n) && !memcmp(trace, want, n * sizeof *want);
    if(!ok){
        failures++;
        fprintf(stderr, "FAIL %s: got [", what);
        for(int i = 0; i < n_trace; i++) fprintf(stderr, "%s%02x", i?" ":"", trace[i]);
        fprintf(stderr, "], want [");
        for(int i = 0; i < n; i++) fprintf(stderr, "%s%02x", i?" ":"", want[i]);
        fprintf(stderr, "]\n");
    }
    n_trace = 0;
}

int main(void){
    /* m68k_call pushes a return address, so it needs real memory */
    M.memsize = 0x10000; M.mem = calloc(1, M.memsize);
    if(!M.mem){ fprintf(stderr, "out of memory\n"); return 1; }
    SP = 0x8000;

    m68k_register(SEG_BASE + 0x00, SEG_BASE + 0x30, fn_a);
    m68k_register(SEG_BASE + 0x30, SEG_BASE + 0x50, fn_b);

    /* a jump to a function start still runs the whole body */
    { const uint32_t w[] = {0x00,0x10,0x20};
      m68k_jump(SEG_BASE + 0x00); expect("jump to start", w, 3); }

    /* ...and a jump into the middle enters there. This is the whole point: it
     * is not a registered start, so only the range search can resolve it. */
    { const uint32_t w[] = {0x20};
      m68k_jump(SEG_BASE + 0x20); expect("jump to interior", w, 1); }

    { const uint32_t w[] = {0x10,0x20};
      m68k_jump(SEG_BASE + 0x10); expect("jump to interior (earlier)", w, 2); }

    /* the address picks the owning function, not merely the first registered */
    { const uint32_t w[] = {0x40};
      m68k_jump(SEG_BASE + 0x40); expect("jump into the second function", w, 1); }

    /* a call, not just a tail jump, and the stack balances across it */
    { const uint32_t w[] = {0x20};
      uint32_t sp = SP;
      m68k_call(SEG_BASE + 0x20); expect("call to interior", w, 1);
      if(SP != sp){ failures++; fprintf(stderr, "FAIL call SP %x -> %x\n", sp, SP); } }

    /* inside a function but not on a boundary: reported, not silently run */
    { const uint32_t w[] = {0};
      fprintf(stderr, "-- expect one 'not an instruction boundary' below --\n");
      m68k_jump(SEG_BASE + 0x18); expect("interior non-boundary", w, 0); }

    /* outside every range: still the old, correct complaint */
    { const uint32_t w[] = {0};
      fprintf(stderr, "-- expect one 'no function at' below --\n");
      m68k_jump(0x900000); expect("unowned address", w, 0); }

    free(M.mem);
    if(failures){ fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("entry-dispatch self-check OK\n");
    return 0;
}

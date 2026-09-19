/* m68k.h - execution substrate for statically-recompiled 68000 Mac code.
 *
 * The lifter (tools/lift68k.py) turns each 68k subroutine into a C function that
 * operates on this state: data/address registers, CCR flags, and a flat
 * big-endian memory. A-line Toolbox traps become m68k_trap() calls handled by
 * the Toolbox HAL; inter-segment jumps resolve through the A5 jump table.
 *
 * Single global CPU (the classic Mac is single-threaded cooperative). */
#ifndef MACRECOMP_M68K_H
#define MACRECOMP_M68K_H

#include <stdint.h>
#include <string.h>

typedef struct M68K {
    uint32_t d[8];     /* D0-D7 */
    uint32_t a[8];     /* A0-A7 (a[7] = SP) */
    uint32_t pc;
    int x, n, z, v, c; /* CCR flags, each 0/1 */
    uint8_t *mem;      /* flat address space (big-endian, Mac layout) */
    uint32_t memsize;
} M68K;

extern M68K M;
#define SP (M.a[7])

/* 0 is a real address a stray pointer writes to, and those writes are worth
 * seeing, so "off" cannot be 0. */
#define MR_WATCH_OFF 0xFFFFFFFFu
extern uint32_t g_watch_addr;
void mr_watch_hit(uint32_t addr, uint32_t val);

/* ---- big-endian memory access (bounds-checked: 24-bit space masked into
 * M.mem; out-of-range reads yield 0 and writes are dropped, so a stray pointer
 * degrades gracefully instead of segfaulting) ---- */
static inline int m68k_ok(uint32_t a){ return a < M.memsize; }
static inline uint32_t m68k_r8 (uint32_t a){ return m68k_ok(a)?M.mem[a]:0; }
static inline uint32_t m68k_r16(uint32_t a){ return ((uint32_t)m68k_r8(a)<<8)|m68k_r8(a+1); }
static inline uint32_t m68k_r32(uint32_t a){ return (m68k_r16(a)<<16)|m68k_r16(a+2); }
static inline void m68k_w8 (uint32_t a,uint32_t v){
    if(m68k_ok(a)) M.mem[a]=(uint8_t)v;
    /* Reported after the store, and the handler reads the whole longword back:
     * the watch sits on byte writes so that it catches everything, but a
     * longword store arrives here one byte at a time and the byte at the
     * watched address is the most significant one. Reporting that byte alone
     * makes every write to a heap pointer look like a write of zero. */
    if (a == g_watch_addr && g_watch_addr != MR_WATCH_OFF) mr_watch_hit(a, v); }
static inline void m68k_w16(uint32_t a,uint32_t v){ m68k_w8(a,v>>8); m68k_w8(a+1,v); }
/* MRWATCHADDR=<hex>: report every longword write to one guest address, with
 * the function that made it. "Who set this global?" is otherwise unanswerable
 * when nothing stores to it at a literal offset -- the write comes through a
 * register, a Toolbox trap, or a struct copy. Zero (the default) costs one
 * compare against a global per 32-bit write. */
static inline void m68k_w32(uint32_t a,uint32_t v){ m68k_w16(a,v>>16); m68k_w16(a+2,v); }

/* ---- sign helpers ---- */
static inline int32_t sx8 (uint32_t v){ return (int8_t)v; }
static inline int32_t sx16(uint32_t v){ return (int16_t)v; }
static inline int32_t sx32(uint32_t v){ return (int32_t)v; }

/* ---- register field access by size (writes preserve the unused high bits) ---- */
#define DB(n)      ((uint8_t)M.d[n])
#define DW(n)      ((uint16_t)M.d[n])
#define DL(n)      (M.d[n])
#define SET_DB(n,v) (M.d[n]=(M.d[n]&0xFFFFFF00u)|((v)&0xFFu))
#define SET_DW(n,v) (M.d[n]=(M.d[n]&0xFFFF0000u)|((v)&0xFFFFu))
#define SET_DL(n,v) (M.d[n]=(uint32_t)(v))

/* ---- flag setters (msb: 0x80/0x8000/0x80000000 chosen by size) ---- */
static inline uint32_t msb(int sz){ return sz==1?0x80u:sz==2?0x8000u:0x80000000u; }
static inline uint32_t szmask(int sz){ return sz==1?0xFFu:sz==2?0xFFFFu:0xFFFFFFFFu; }

static inline void fl_logic(uint32_t r,int sz){ /* move,and,or,eor,not,tst,clr */
    r&=szmask(sz); M.n=(r&msb(sz))!=0; M.z=(r==0); M.v=0; M.c=0;
}
static inline void fl_add(uint32_t s,uint32_t d,uint32_t r,int sz){
    uint32_t m=msb(sz); s&=szmask(sz); d&=szmask(sz); r&=szmask(sz);
    M.n=(r&m)!=0; M.z=(r==0);
    M.v=(((s^r)&(d^r))&m)!=0;
    M.c=M.x=(((s&d)|(~r&d)|(s&~r))&m)!=0;
}
static inline void fl_sub(uint32_t s,uint32_t d,uint32_t r,int sz){ /* d - s = r */
    uint32_t m=msb(sz); s&=szmask(sz); d&=szmask(sz); r&=szmask(sz);
    M.n=(r&m)!=0; M.z=(r==0);
    M.v=(((s^d)&(d^r))&m)!=0;
    M.c=M.x=(((s&~d)|(r&~d)|(s&r))&m)!=0;
}
static inline void fl_cmp(uint32_t s,uint32_t d,uint32_t r,int sz){ /* like sub but X unchanged */
    int savex=M.x; fl_sub(s,d,r,sz); M.x=savex;
}

/* ---- multi-precision arithmetic (the X flag) ----
 * addx/subx/negx carry the X flag in and out, and their Z is **sticky**: it is
 * cleared by a non-zero result and otherwise left alone, so a multi-word add
 * ends with Z set only when every word was zero. Setting Z the ordinary way
 * would make the top word alone decide it. */
static inline void fl_addx(uint32_t s,uint32_t d,uint32_t r,int sz){
    uint32_t m=msb(sz); s&=szmask(sz); d&=szmask(sz); r&=szmask(sz);
    M.n=(r&m)!=0;
    if(r) M.z=0;
    M.v=(((s^r)&(d^r))&m)!=0;
    M.c=M.x=(((s&d)|(~r&d)|(s&~r))&m)!=0;
}
static inline void fl_subx(uint32_t s,uint32_t d,uint32_t r,int sz){ /* d - s - x */
    uint32_t m=msb(sz); s&=szmask(sz); d&=szmask(sz); r&=szmask(sz);
    M.n=(r&m)!=0;
    if(r) M.z=0;
    M.v=(((s^d)&(d^r))&m)!=0;
    M.c=M.x=(((s&~d)|(r&~d)|(s&r))&m)!=0;
}

/* ---- shifts (flag-setting) ---- */
static inline uint32_t m68k_lsl(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz); v&=m;
    if(c){ M.c=M.x=(v>>(sz*8-c))&1; v=(v<<c)&m; } else M.c=0;
    M.n=(v&msb(sz))!=0; M.z=(v==0); M.v=0; return v;
}
static inline uint32_t m68k_lsr(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz); v&=m;
    if(c){ M.c=M.x=(v>>(c-1))&1; v=(v>>c)&m; } else M.c=0;
    M.n=(v&msb(sz))!=0; M.z=(v==0); M.v=0; return v;
}
/* ASL's V is set if the sign bit changed at ANY point during the shift, not
 * merely if the first and last signs differ: 0x40000000 shifted left twice
 * passes through 0x80000000 and back to 0, so V is set even though it starts
 * and ends positive. Comparing only the endpoints misses exactly the cases V
 * exists to catch, and every signed branch after an ASL then takes the wrong
 * arm. Shifting one bit at a time is what makes that observable; counts are
 * 0-63 so the loop is cheap. */
static inline uint32_t m68k_asl(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz),mb=msb(sz); v&=m; uint32_t sign=v&mb; int ov=0;
    if(c){ for(int i=0;i<c;i++){ M.c=M.x=(v&mb)!=0; v=(v<<1)&m;
                                 if((v&mb)!=sign) ov=1; } }
    else M.c=0;
    M.n=(v&mb)!=0; M.z=(v==0); M.v=ov; return v;
}
static inline uint32_t m68k_asr(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz),mb=msb(sz); v&=m; uint32_t sign=v&mb;
    if(c){ M.c=M.x=(v>>(c-1))&1; for(int i=0;i<c;i++) v=(v>>1)|sign; v&=m; } else M.c=0;
    M.n=(v&mb)!=0; M.z=(v==0); M.v=0; return v;
}
/* ---- rotates ----
 * ROL/ROR, not ROXL/ROXR: the bit leaves one end and re-enters the other, and
 * **X is not affected** -- only C, from the last bit rotated out. Rotating one
 * bit at a time rather than by a shift-pair keeps the wrap cases right: ROL.B
 * by 8 returns the value unchanged but still sets C from the original bit 7,
 * which a `(v<<r)|(v>>(w-r))` form gets wrong when r is 0. Counts are 0-63, so
 * the loop is cheap. */
static inline uint32_t m68k_rol(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz),mb=msb(sz); v&=m; c&=63;
    if(c){ for(int i=0;i<c;i++){ M.c=(v&mb)!=0; v=((v<<1)|(uint32_t)M.c)&m; } } else M.c=0;
    M.n=(v&mb)!=0; M.z=(v==0); M.v=0; return v;
}
static inline uint32_t m68k_ror(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz),mb=msb(sz); v&=m; c&=63;
    if(c){ for(int i=0;i<c;i++){ M.c=v&1; v=(v>>1)|(M.c?mb:0); } } else M.c=0;
    M.n=(v&mb)!=0; M.z=(v==0); M.v=0; return v;
}

/* ROXL/ROXR rotate *through* X, so the register and X together form a 9, 17 or
 * 33 bit ring. With a zero count C takes X's value rather than being cleared,
 * which is the one case that separates them from ROL/ROR. */
static inline uint32_t m68k_roxl(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz),mb=msb(sz); v&=m; c&=63;
    if(c){ for(int i=0;i<c;i++){ int nx=(v&mb)!=0; v=((v<<1)|(uint32_t)M.x)&m; M.x=nx; } }
    M.c=M.x;
    M.n=(v&mb)!=0; M.z=(v==0); M.v=0; return v;
}
static inline uint32_t m68k_roxr(uint32_t v,int c,int sz){
    uint32_t m=szmask(sz),mb=msb(sz); v&=m; c&=63;
    if(c){ for(int i=0;i<c;i++){ int nx=v&1; v=(v>>1)|(M.x?mb:0); M.x=nx; } }
    M.c=M.x;
    M.n=(v&mb)!=0; M.z=(v==0); M.v=0; return v;
}

/* ---- Toolbox trap dispatch (implemented by the HAL) ---- */
void m68k_trap(uint16_t word);     /* word = the full A-line opcode ($Axxx) */

/* ---- function table: original 24-bit code address -> lifted C function ----
 *
 * A lifted function takes an `entry` address so it can be entered part-way. 0
 * means "run from the top"; any other value is an absolute code address inside
 * the function, which its prologue turns into a goto. Computed jumps in the
 * original code land mid-function, and a table of function *starts* alone
 * cannot express that. */
typedef void (*m68k_fn)(uint32_t entry);
void m68k_register(uint32_t start, uint32_t end, m68k_fn fn);  /* [start,end) */
void m68k_call(uint32_t addr);     /* resolve + invoke (jsr/bsr to a known target) */
void m68k_jump(uint32_t addr);     /* tail transfer (jmp): no return address pushed */
void m68k_entry_miss(uint32_t entry);  /* lifted prologue: address is not a boundary */

/* The last two lifted functions entered, and the call depth. Kept for
 * debugging: at any trap, g_last_call names the function that made it, which is
 * the cheapest way to locate a title that then stops making progress. */
extern volatile uint32_t g_last_call, g_prev_call;
extern volatile uint32_t g_shadow[512];
extern volatile int g_shadow_sp;
void m68k_rts(void);               /* rts: pop the sentinel return address if present */
void m68k_jt_call(uint32_t a5off); /* call through the A5 jump table (jsr d(a5)) */
void m68k_jt_jump(uint32_t a5off); /* tail jmp through the A5 jump table */
void m68k_jt_set(uint32_t a5off, uint32_t addr); /* loader wires a5 offset -> code addr */

/* trap raised by the lifter for an instruction it could not translate */
void m68k_unimplemented(const char *what, uint32_t addr);

/* ---- backward-branch hook ----
 *
 * The lifter emits MR_LOOPTICK at every backward branch, because that is the
 * one thing every loop in the original code has to go round. It does two jobs.
 *
 * 1. **Time has to pass inside a loop.** Classic Mac code busy-waits on the
 *    low-memory Ticks global (0x16A) -- HyperCard spins on it to calibrate
 *    machine speed, and games wait on it for timing. A real Mac advanced Ticks
 *    from the VBL interrupt; this runtime has no interrupt, and a loop that
 *    spins on Ticks makes no trap, call or tail jump, so nothing else in the
 *    runtime ever gets to run. Without this the wait can only be infinite.
 *    The counter is decremented inline and calls out rarely, so an ordinary
 *    loop pays an add and a branch.
 *
 * 2. **Finding a spin.** Built with -DMACRECOMP_LOOPGUARD and run with
 *    MRMAXLOOPS=<n>, it also counts branches by address and prints the hottest
 *    on budget, which names the looping instruction outright. That census is
 *    debug-only; the time slice above is not. */
extern int mr_tickdown;
void m68k_time_slice(void);        /* refill the counter and advance Ticks */

#ifdef MACRECOMP_LOOPGUARD
void m68k_loop_tick(uint32_t pc);
#define MR_LOOPTICK(pc) \
    do{ m68k_loop_tick(g_seg_base + (pc)); if(--mr_tickdown <= 0) m68k_time_slice(); }while(0)
#else
#define MR_LOOPTICK(pc) do{ if(--mr_tickdown <= 0) m68k_time_slice(); }while(0)
#endif

#endif /* MACRECOMP_M68K_H */

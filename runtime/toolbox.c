/* toolbox.c - Toolbox trap dispatch (m68k_trap) -> QuickDraw/Event/Memory HAL.
 *
 * Toolbox (Pascal) traps read arguments the lifted code pushed onto the 68k
 * stack (pointers reference M.mem); results are pushed back. OS traps (Memory
 * Manager) use registers (D0/A0). Only the traps Shufflepuck exercises are
 * implemented; the rest log once so the boot trace is legible. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include <stdio.h>
#include <string.h>

/* ---- Pascal stack helpers ---- */
static uint16_t pop16(void){ uint16_t v=(uint16_t)m68k_r16(SP); SP+=2; return v; }
static uint32_t pop32(void){ uint32_t v=m68k_r32(SP); SP+=4; return v; }
static void push16(uint16_t v){ SP-=2; m68k_w16(SP,v); }
static void push32(uint32_t v){ SP-=4; m68k_w32(SP,v); }

static Rect rd_rect(uint32_t p){ Rect r; r.top=(int16_t)m68k_r16(p); r.left=(int16_t)m68k_r16(p+2);
    r.bottom=(int16_t)m68k_r16(p+4); r.right=(int16_t)m68k_r16(p+6); return r; }
static void wr_rect(uint32_t p,const Rect*r){ m68k_w16(p,r->top); m68k_w16(p+2,r->left);
    m68k_w16(p+4,r->bottom); m68k_w16(p+6,r->right); }

/* ---- bump heap inside M.mem (NewPtr/NewHandle) ---- */
static uint32_t heap_ptr = 0, heap_end = 0;
static void heap_init(void){ heap_ptr = 0x200000; heap_end = 0x600000; }  /* 2MB..6MB */
static uint32_t heap_alloc(uint32_t sz){ sz=(sz+3)&~3u; if(heap_ptr+sz>=heap_end) return 0;
    uint32_t p=heap_ptr; heap_ptr+=sz; for(uint32_t i=0;i<sz;i++) M.mem[p+i]=0; return p; }

/* ---- trap normalization: strip auto-pop / flag bits ---- */
static uint16_t norm(uint16_t w){ return (w&0x0800)?(0xA800|(w&0x03FF)):(0xA000|(w&0x00FF)); }

static int g_inited = 0;
void toolbox_init(void){ qd_init(); heap_init(); g_inited=1; }

static void logtrap(uint16_t w){
    static uint16_t seen[512]; static int ns=0;
    for(int i=0;i<ns;i++) if(seen[i]==w) return;
    if(ns<512) seen[ns++]=w;
    fprintf(stderr,"trap $%04X unimplemented\n", w);
}

void m68k_trap(uint16_t raw){
    uint16_t w = norm(raw);
    switch(w){
    /* ---- init (mostly no-ops for us) ---- */
    case 0xA86E: /*InitGraf*/ (void)pop32(); break;      /* arg: globalsPtr */
    case 0xA8FE: /*InitFonts*/ case 0xA912: /*InitWindows*/ case 0xA930: /*InitMenus*/
    case 0xA9CC: /*TEInit*/    case 0xA063: /*MaxApplZone*/ case 0xA850: /*InitCursor*/
    case 0xA036: /*MoreMasters*/ break;
    case 0xA97B: /*InitDialogs*/ (void)pop32(); break;   /* arg: resumeProc */
    case 0xA032: /*FlushEvents*/ (void)pop32(); break;

    /* ---- QuickDraw: pen & text state ---- */
    case 0xA873: /*SetPort*/ (void)pop32(); break;
    case 0xA874: /*GetPort*/ { uint32_t pp=pop32(); if(pp) m68k_w32(pp,0); } break;
    case 0xA89E: /*PenNormal*/ qd_pen_size(1,1); qd_pen_mode(0); qd_pen_pat_black(1); break;
    case 0xA89B: /*PenSize*/ { int16_t h=pop16(),ww=pop16(); qd_pen_size(ww,h); } break;
    case 0xA89C: /*PenMode*/ qd_pen_mode(pop16()); break;
    case 0xA89D: /*PenPat*/ (void)pop32(); qd_pen_pat_black(1); break;
    case 0xA887: /*TextFont*/ case 0xA888: /*TextFace*/ case 0xA88A: /*TextSize*/
    case 0xA889: /*TextMode*/ (void)pop16(); break;

    /* ---- QuickDraw: rect utilities ---- */
    case 0xA8A7: /*SetRect*/ { int16_t b=pop16(),r=pop16(),t=pop16(),l=pop16(); uint32_t rp=pop32();
        Rect rr; rect_set(&rr,l,t,r,b); wr_rect(rp,&rr); } break;
    case 0xA8A8: /*OffsetRect*/ { int16_t dv=pop16(),dh=pop16(); uint32_t rp=pop32();
        Rect rr=rd_rect(rp); rect_offset(&rr,dh,dv); wr_rect(rp,&rr); } break;
    case 0xA8A9: /*InsetRect*/ { int16_t dv=pop16(),dh=pop16(); uint32_t rp=pop32();
        Rect rr=rd_rect(rp); rect_inset(&rr,dh,dv); wr_rect(rp,&rr); } break;
    case 0xA8AA: /*SectRect*/ { uint32_t dst=pop32(),b=pop32(),a=pop32();
        Rect ra=rd_rect(a),rb=rd_rect(b),o; int nz=rect_sect(&ra,&rb,&o); if(nz)wr_rect(dst,&o);
        push16(nz?1:0); } break;
    case 0xA8AB: /*UnionRect*/ { uint32_t dst=pop32(),b=pop32(),a=pop32();
        Rect ra=rd_rect(a),rb=rd_rect(b),o; rect_union(&ra,&rb,&o); wr_rect(dst,&o); } break;
    case 0xA8AD: /*PtInRect*/ { uint32_t rp=pop32(); uint32_t pt=pop32();
        int16_t v=(int16_t)m68k_r16(pt),h=(int16_t)m68k_r16(pt+2); Rect rr=rd_rect(rp);
        push16(pt_in_rect(h,v,&rr)?1:0); } break;

    /* ---- QuickDraw: drawing ---- */
    case 0xA893: /*MoveTo*/ { int16_t v=pop16(),h=pop16(); qd_pen_to(h,v); } break;
    case 0xA894: /*Move*/   { int16_t dv=pop16(),dh=pop16(); qd_pen_to(0,0); (void)dh;(void)dv; } break;
    case 0xA891: /*LineTo*/ { int16_t v=pop16(),h=pop16(); qd_line_to(h,v); } break;
    case 0xA892: /*Line*/   { int16_t dv=pop16(),dh=pop16(); qd_line(dh,dv); } break;
    case 0xA8A1: /*FrameRect*/ { Rect r=rd_rect(pop32()); qd_frame_rect(&r); } break;
    case 0xA8A2: /*PaintRect*/ { Rect r=rd_rect(pop32()); qd_paint_rect(&r); } break;
    case 0xA8A3: /*EraseRect*/ { Rect r=rd_rect(pop32()); qd_erase_rect(&r); } break;
    case 0xA8A4: /*InverRect*/ { Rect r=rd_rect(pop32()); qd_invert_rect(&r); } break;
    case 0xA8A5: /*FillRect*/  { (void)pop32(); Rect r=rd_rect(pop32()); qd_fill_rect(&r,1); } break;
    case 0xA8B6: /*FrameOval*/ { Rect r=rd_rect(pop32()); qd_frame_oval(&r); } break;
    case 0xA8B7: /*PaintOval*/ { Rect r=rd_rect(pop32()); qd_fill_oval(&r,1); } break;
    case 0xA8BB: /*FillOval*/  { (void)pop32(); Rect r=rd_rect(pop32()); qd_fill_oval(&r,1); } break;
    case 0xA87B: /*ClipRect*/  { Rect r=rd_rect(pop32()); qd_set_clip(&r); } break;
    case 0xA884: /*DrawString*/{ uint32_t s=pop32(); int len=m68k_r8(s); uint8_t buf[256];
        for(int i=0;i<len;i++) buf[i]=(uint8_t)m68k_r8(s+1+i); qd_draw_text(buf,len); } break;
    case 0xA883: /*DrawChar*/  { qd_draw_char(pop16()); } break;

    /* ---- events (thin) ---- */
    case 0xA975: /*TickCount*/ push32(plat_ticks()); break;
    case 0xA972: /*GetMouse*/ { uint32_t pt=pop32(); int h,v; plat_get_mouse(&h,&v);
        m68k_w16(pt,v); m68k_w16(pt+2,h); } break;
    case 0xA974: /*Button*/ push16(plat_button()?1:0); break;
    case 0xA973: /*StillDown*/ push16(plat_button()?1:0); break;
    case 0xA970: /*GetNextEvent*/ case 0xA971: /*EventAvail*/ {
        (void)pop16(); uint32_t evp=pop32(); int what=0,msg=0,h=0,v=0;
        plat_pump(); int got=plat_next_event(&what,&msg,&h,&v);
        if(evp){ m68k_w16(evp,what); m68k_w32(evp+2,msg); m68k_w32(evp+6,plat_ticks());
                 int mh,mv; plat_get_mouse(&mh,&mv); m68k_w16(evp+10,mv); m68k_w16(evp+12,mh);
                 m68k_w16(evp+14,0); }
        push16(got?1:0); } break;

    /* ---- Memory Manager (register-based) ---- */
    case 0xA11E: /*NewPtr*/ case 0xA51E: /*NewPtrSys*/ { M.a[0]=heap_alloc(M.d[0]); M.d[0]=M.a[0]?0:-108; } break;
    case 0xA122: /*NewHandle*/ { uint32_t p=heap_alloc(M.d[0]); uint32_t hh=heap_alloc(4);
        if(hh)m68k_w32(hh,p); M.a[0]=hh; M.d[0]=hh?0:-108; } break;
    case 0xA01F: /*DisposePtr*/ case 0xA023: /*DisposeHandle*/ M.d[0]=0; break;
    case 0xA02E: /*BlockMove*/ { uint32_t src=M.a[0],dst=M.a[1],n=M.d[0];
        for(uint32_t i=0;i<n;i++) M.mem[dst+i]=M.mem[src+i]; } break;
    case 0xA029: /*HLock*/ case 0xA02A: /*HUnlock*/ case 0xA02B: /*EmptyHandle*/ break;

    default: logtrap(w); break;
    }
}

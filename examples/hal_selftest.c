/* hal_selftest.c - drive the Toolbox HAL through real trap calls.
 *
 * Pushes Pascal arguments onto the 68k stack exactly as lifted code would, then
 * calls m68k_trap(). Draws a mock "Shufflepuck" table so the whole path
 * (Pascal args -> dispatch -> QuickDraw -> framebuffer) is exercised, and dumps
 * the 1-bit framebuffer to a PGM for inspection. No SDL/display needed. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include <stdio.h>
#include <stdlib.h>

/* headless stubs for the platform layer (real one is platform_sdl.c) */
uint32_t plat_ticks(void){ return 0; }
void plat_get_mouse(int*h,int*v){ if(h)*h=0; if(v)*v=0; }
int  plat_button(void){ return 0; }
void plat_pump(void){}
int  plat_next_event(int*w,int*m,int*h,int*v){ (void)m;(void)h;(void)v; if(w)*w=0; return 0; }

static void push16(uint16_t v){ SP-=2; m68k_w16(SP,v); }
static void push32(uint32_t v){ SP-=4; m68k_w32(SP,v); }

/* build a Rect in scratch mem, return its 68k address */
static uint32_t SCRATCH = 0x080000;
static uint32_t rect(int l,int t,int r,int b){ uint32_t p=SCRATCH; SCRATCH+=8;
    m68k_w16(p,t); m68k_w16(p+2,l); m68k_w16(p+4,b); m68k_w16(p+6,r); return p; }

#define TRAP(w) m68k_trap(w)

int main(void){
    M.memsize = 8u*1024*1024; M.mem = calloc(1,M.memsize);
    SP = 0x100000;                       /* 68k stack */
    toolbox_init();

    /* erase to white */
    { uint32_t r=rect(0,0,QD_W,QD_H); push32(r); TRAP(0xA8A3); }   /* EraseRect */

    /* table border (FrameRect) */
    { uint32_t r=rect(20,20,QD_W-20,QD_H-20); push32(r); TRAP(0xA8A1); }

    /* center line (MoveTo/LineTo) */
    push16(QD_W/2); push16(20); TRAP(0xA893);         /* MoveTo(h=256,v=20) */
    push16(QD_W/2); push16(QD_H-20); TRAP(0xA891);    /* LineTo */

    /* two paddles (PaintRect) */
    { uint32_t r=rect(60,150,110,192); push32(r); TRAP(0xA8A2); }
    { uint32_t r=rect(QD_W-110,150,QD_W-60,192); push32(r); TRAP(0xA8A2); }

    /* the puck (PaintOval via FillOval) */
    { uint32_t r=rect(240,160,272,192); push32(r); m68k_w32(SP-4,0); SP-=4; /*pat*/ push32(r); TRAP(0xA8BB); }

    /* a scoreboard-ish string */
    push16(40); push16(40); TRAP(0xA893);             /* MoveTo */
    { uint32_t s=0x090000; const char *t="\x0aSHUFFLEPUK"; for(int i=0;i<11;i++) m68k_w8(s+i,(uint8_t)t[i]);
      push32(s); TRAP(0xA884); }                       /* DrawString (Pascal string) */

    /* dump framebuffer as PGM (P5) */
    FILE *f=fopen("hal_out.pgm","wb");
    fprintf(f,"P5\n%d %d\n255\n",QD_W,QD_H);
    for(int y=0;y<QD_H;y++) for(int x=0;x<QD_W;x++){ unsigned char c=qd_fb[y][x]?0:255; fputc(c,f);}
    fclose(f);
    printf("HAL selftest: drew scene via traps -> hal_out.pgm\n");
    return 0;
}

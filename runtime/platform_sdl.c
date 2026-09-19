/* platform_sdl.c - SDL2 window + input for the Toolbox HAL.
 * Presents the 1-bit QuickDraw framebuffer (black on white) scaled up, and
 * turns SDL input into a tiny event queue the Event Manager shim reads. */
#include "macrecomp/toolbox.h"
#include "macrecomp/m68k.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static int g_scale = 2;
static int quit_req = 0;
static uint32_t start_ms;

/* minimal event ring */
#define EVQ 64
static struct { int what, msg, h, v; } evq[EVQ];
static int evhead, evtail;
static void evpush(int what,int msg,int h,int v){
    int n=(evtail+1)%EVQ; if(n==evhead) return; evq[evtail].what=what; evq[evtail].msg=msg;
    evq[evtail].h=h; evq[evtail].v=v; evtail=n;
}

int plat_open(const char *title, int scale){
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0){ fprintf(stderr,"SDL:%s\n",SDL_GetError()); return 1; }
    g_scale = scale>0?scale:2;
    win = SDL_CreateWindow(title?title:"macrecomp", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, QD_W*g_scale, QD_H*g_scale, SDL_WINDOW_SHOWN);
    if(!win){ fprintf(stderr,"win:%s\n",SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, QD_W, QD_H);
    start_ms = SDL_GetTicks();
    return (ren&&tex)?0:1;
}

/* MRSHOT=<path> writes the framebuffer to a PGM on every present (overwriting),
 * so a run can be inspected without a display -- useful for CI and for capturing
 * a frame from a title that is sitting in a modal loop. */
static int screen_px(int x, int y);
static void shot(void){
    const char *path = getenv("MRSHOT");
    if(!path) return;
    /* Skip an all-white frame. Every present overwrites this file, so a title
     * that clears the screen on the way out would otherwise replace the one
     * frame worth keeping with a blank one. */
    {   int any=0;
        for(int y=0;y<QD_H&&!any;y++) for(int x=0;x<QD_W;x++) if(screen_px(x,y)){ any=1; break; }
        if(!any) return; }
    /* Write then rename: this runs on every present, so a run killed by a
     * timeout would otherwise leave a half-written file exactly when the
     * picture is wanted. */
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if(!f) return;
    fprintf(f, "P5\n%d %d\n255\n", QD_W, QD_H);
    for(int y=0;y<QD_H;y++) for(int x=0;x<QD_W;x++) fputc(screen_px(x,y) ? 0 : 255, f);
    fclose(f);
    remove(path); rename(tmp, path);
}

/* MRBMSHOT=<hexbase>:<rowbytes>:<height>:<path> writes a 1-bit bitmap out of
 * guest memory as a PGM. A title that draws its card into an offscreen buffer
 * and never blits it leaves the screen blank, and a blank screen cannot tell
 * you whether the drawing happened at all. This can. */
static void bmshot(void){
    const char *spec = getenv("MRBMSHOT");
    if(!spec) return;
    /* MRBMSHOT=scan: report the densest 4 KB blocks of the heap. "Nothing is on
     * screen" and "nothing was drawn anywhere" look identical from the
     * framebuffer; this tells them apart without guessing a buffer address. */
    if(!strcmp(spec, "scan")){
        static int n; if(++n != 400) return;   /* late enough to see the end state */
        struct { unsigned base, bits; } top[8] = {{0,0}};
        for(unsigned a = 0x800000u; a + 4096 < M.memsize && a < 0x1000000u; a += 4096){
            unsigned bits = 0;
            for(unsigned k = 0; k < 4096; k++){
                unsigned char c = M.mem[a+k];
                while(c){ bits += c & 1; c >>= 1; } }
            for(int i = 0; i < 8; i++) if(bits > top[i].bits){
                for(int j = 7; j > i; j--) top[j] = top[j-1];
                top[i].base = a; top[i].bits = bits; break; } }
        fprintf(stderr, "[bmscan] densest 4K blocks (of 32768 bits):\n");
        for(int i = 0; i < 8; i++) if(top[i].bits)
            fprintf(stderr, "  %06x  %u bits set (%.1f%%)\n",
                    top[i].base, top[i].bits, 100.0*top[i].bits/32768.0);
        return;
    }
    unsigned base=0, rb=0, h=0; char path[256];
    if(sscanf(spec, "%x:%u:%u:%255s", &base, &rb, &h, path) != 4) return;
    if(!rb || !h || rb > 4096 || h > 4096) return;
    /* Write then rename: this runs on every present, so a run ended by a
     * timeout would otherwise leave a half-written file exactly when the
     * picture is wanted. */
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if(!f) return;
    fprintf(f, "P5\n%u %u\n255\n", rb*8, h);
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<rb*8u;x++){
        unsigned a = base + y*rb + (x>>3);
        int bit = a < M.memsize ? (M.mem[a] >> (7-(x&7))) & 1 : 0;
        fputc(bit ? 0 : 255, f); }
    fclose(f);
    remove(path); rename(tmp, path);
}

/* A pixel is set if QuickDraw drew it into qd_fb or the title blitted it
 * straight into screen memory. HyperCard paints its card with its own blitter,
 * so showing qd_fb alone leaves the card invisible however well it rendered. */
static int screen_px(int x, int y){
    if(qd_fb[y][x]) return 1;
    uint32_t base = mr_screen_base();
    if(!base) return 0;
    uint32_t a = base + (uint32_t)y * (QD_W/8) + (uint32_t)(x >> 3);
    return a < M.memsize ? (M.mem[a] >> (7 - (x & 7))) & 1 : 0;
}

void plat_present(void){
    shot(); bmshot();
    if(!tex) return;
    uint32_t *px; int pitch;
    SDL_LockTexture(tex, NULL, (void**)&px, &pitch);
    for(int y=0;y<QD_H;y++){ uint32_t *row=(uint32_t*)((uint8_t*)px+y*pitch);
        for(int x=0;x<QD_W;x++) row[x] = screen_px(x,y) ? 0xFF000000u : 0xFFFFFFFFu; }
    SDL_UnlockTexture(tex);
    SDL_RenderClear(ren); SDL_RenderCopy(ren,tex,NULL,NULL); SDL_RenderPresent(ren);
}

void plat_pump(void){
    SDL_Event e;
    while(SDL_PollEvent(&e)){
        /* Nothing polls plat_quit_requested, so closing the window has to
         * act here or the title keeps running with no way to stop it. */
        if(e.type==SDL_QUIT){ quit_req=1; plat_shutdown(); exit(0); }
        else if(e.type==SDL_MOUSEBUTTONDOWN) evpush(1/*mouseDown*/,0,e.button.x/g_scale,e.button.y/g_scale);
        else if(e.type==SDL_MOUSEBUTTONUP)   evpush(2/*mouseUp*/,0,e.button.x/g_scale,e.button.y/g_scale);
        else if(e.type==SDL_KEYDOWN){ if(e.key.keysym.sym==SDLK_ESCAPE) quit_req=1;
                                      evpush(3/*keyDown*/,e.key.keysym.sym,0,0); }
    }
}

int plat_quit_requested(void){ return quit_req; }
void plat_get_mouse(int *h,int *v){ int x,y; SDL_GetMouseState(&x,&y); if(h)*h=x/g_scale; if(v)*v=y/g_scale; }
int plat_button(void){ return (SDL_GetMouseState(NULL,NULL)&SDL_BUTTON(SDL_BUTTON_LEFT))!=0; }
uint32_t plat_ticks(void){ return (SDL_GetTicks()-start_ms)*60u/1000u; }

int plat_next_event(int *what,int *msg,int *h,int *v){
    if(evhead==evtail){
        /* MRKEYS=1: answer modal dialogs with Return so an unattended run keeps
         * going. Off by default here -- with a window open there is a person to
         * click, and a synthetic keypress would fight them for the dialog. */
        /* MRCLICK=x,y[,n]: synthesise a click at x,y after n polls (default
         * 4000). Lets an unattended run exercise a title's own buttons, which
         * is the only way to show that anything is actually navigable without
         * a person at the window. */
        {   static int cx=-1, cy, cwhen, fired; static long cpolls;
            if(cx < 0){ const char *e=getenv("MRCLICK");
                cx = 0; cwhen = 4000;
                if(e) sscanf(e, "%d,%d,%d", &cx, &cy, &cwhen); }
            if(cx > 0 && !fired && ++cpolls >= cwhen){
                fired = 1;
                evpush(1/*mouseDown*/, 0, cx, cy);
                evpush(2/*mouseUp*/,   0, cx, cy);
                fprintf(stderr, "[click] %d,%d\n", cx, cy);
            } }
        static int on=-1; static long polls;
        if(on<0){ const char *e=getenv("MRKEYS"); on = e?atoi(e):0; }
        if(on && ++polls % 3000 == 0){
            if(what)*what=3; if(msg)*msg=13; if(h)*h=0; if(v)*v=0; return 1; }
        if(what)*what=0; return 0; }
    if(what)*what=evq[evhead].what; if(msg)*msg=evq[evhead].msg;
    if(h)*h=evq[evhead].h; if(v)*v=evq[evhead].v; evhead=(evhead+1)%EVQ; return 1;
}

void plat_shutdown(void){
    if(tex)SDL_DestroyTexture(tex); if(ren)SDL_DestroyRenderer(ren);
    if(win)SDL_DestroyWindow(win); SDL_Quit();
}

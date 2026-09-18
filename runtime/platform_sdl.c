/* platform_sdl.c - SDL2 window + input for the Toolbox HAL.
 * Presents the 1-bit QuickDraw framebuffer (black on white) scaled up, and
 * turns SDL input into a tiny event queue the Event Manager shim reads. */
#include "macrecomp/toolbox.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

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
static void shot(void){
    const char *path = getenv("MRSHOT");
    if(!path) return;
    /* Skip an all-white frame. Every present overwrites this file, so a title
     * that clears the screen on the way out would otherwise replace the one
     * frame worth keeping with a blank one. */
    {   int any=0;
        for(int y=0;y<QD_H&&!any;y++) for(int x=0;x<QD_W;x++) if(qd_fb[y][x]){ any=1; break; }
        if(!any) return; }
    FILE *f = fopen(path, "wb");
    if(!f) return;
    fprintf(f, "P5\n%d %d\n255\n", QD_W, QD_H);
    for(int y=0;y<QD_H;y++) for(int x=0;x<QD_W;x++) fputc(qd_fb[y][x] ? 0 : 255, f);
    fclose(f);
}

void plat_present(void){
    shot();
    if(!tex) return;
    uint32_t *px; int pitch;
    SDL_LockTexture(tex, NULL, (void**)&px, &pitch);
    for(int y=0;y<QD_H;y++){ uint32_t *row=(uint32_t*)((uint8_t*)px+y*pitch);
        for(int x=0;x<QD_W;x++) row[x] = qd_fb[y][x] ? 0xFF000000u : 0xFFFFFFFFu; }
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

/* quickdraw.c - a small, faithful-enough QuickDraw over a 1-bit framebuffer.
 * Covers what a single-window B&W game needs: pen lines, rects, ovals, fills,
 * text, and CopyBits. 0 = white, 1 = black (we keep 1 byte/pixel for clarity;
 * the SDL layer packs to RGBA). */
#include "macrecomp/toolbox.h"
#include "macrecomp/m68k.h"
#include "font5x7.h"
#include <string.h>
#include <stdlib.h>

uint8_t qd_fb[QD_H][QD_W];

static int pen_h, pen_v, pen_w = 1, pen_h_sz = 1;
static int pen_black = 1;          /* current pen pattern: 1 black, 0 white */
static int pen_mode = 0;           /* 0 = patCopy (srcCopy-ish) */
static Rect clip = { 0, 0, QD_H, QD_W };

/* current drawing target: the screen (qd_fb) or a 1-bit BitMap in guest memory.
 * The game double-buffers -- SetPortBits to an offscreen buffer, draw, then
 * CopyBits it to the screen. */
static struct { int is_screen; uint32_t base; int rowbytes, bl, bt; }
    cur = { 1, 0, 0, 0, 0 };
void qd_set_port(int is_screen, uint32_t base, int rowbytes, int bl, int bt){
    cur.is_screen=is_screen; cur.base=base; cur.rowbytes=rowbytes; cur.bl=bl; cur.bt=bt;
}

static int clamp(int x, int lo, int hi){ return x<lo?lo:x>hi?hi:x; }

static void put(int h, int v, int black){
    if (h < clip.left || h >= clip.right || v < clip.top || v >= clip.bottom) return;
    if (cur.is_screen){
        if (h < 0 || h >= QD_W || v < 0 || v >= QD_H) return;
        qd_fb[v][h] = (uint8_t)black;
    } else {                          /* packed 1-bit bitmap in M.mem (1 = black) */
        int lx = h - cur.bl, ly = v - cur.bt;
        if (lx < 0 || ly < 0 || cur.rowbytes <= 0) return;
        uint32_t a = cur.base + (uint32_t)ly * cur.rowbytes + (lx >> 3);
        uint8_t byte = (uint8_t)m68k_r8(a), mask = 0x80u >> (lx & 7);
        m68k_w8(a, black ? (byte | mask) : (byte & (uint8_t)~mask));
    }
}
static int getpix(int h, int v){
    if (cur.is_screen){
        if (h < 0 || h >= QD_W || v < 0 || v >= QD_H) return 0;
        return qd_fb[v][h];
    }
    int lx = h - cur.bl, ly = v - cur.bt;
    if (lx < 0 || ly < 0 || cur.rowbytes <= 0) return 0;
    return (m68k_r8(cur.base + (uint32_t)ly*cur.rowbytes + (lx>>3)) >> (7-(lx&7))) & 1;
}

void qd_init(void){ memset(qd_fb, 0, sizeof qd_fb); pen_h=pen_v=0; pen_w=pen_h_sz=1;
                    pen_black=1; pen_mode=0; rect_set(&clip,0,0,QD_W,QD_H); }
void qd_set_clip(const Rect *r){ if(r) clip=*r; }
void qd_get_clip(Rect *r){ if(r) *r=clip; }
void qd_pen_size(int w, int h){ pen_w=w>0?w:1; pen_h_sz=h>0?h:1; }
void qd_pen_mode(int mode){ pen_mode=mode; }
void qd_pen_pat_black(int black){ pen_black=black?1:0; }
void qd_pen_to(int h, int v){ pen_h=h; pen_v=v; }

static void hspan(int x0, int x1, int y, int black){
    if (x0>x1){ int t=x0;x0=x1;x1=t; } for(int x=x0;x<=x1;x++) put(x,y,black);
}
static void thick(int h, int v, int black){
    for(int dy=0;dy<pen_h_sz;dy++) for(int dx=0;dx<pen_w;dx++) put(h+dx,v+dy,black);
}

void qd_line_to(int h, int v){
    /* Bresenham from (pen_h,pen_v) to (h,v) */
    int x0=pen_h,y0=pen_v,x1=h,y1=v;
    int dx=abs(x1-x0), dy=-abs(y1-y0), sx=x0<x1?1:-1, sy=y0<y1?1:-1, e=dx+dy;
    for(;;){ thick(x0,y0,pen_black); if(x0==x1&&y0==y1)break;
        int e2=2*e; if(e2>=dy){e+=dy;x0+=sx;} if(e2<=dx){e+=dx;y0+=sy;} }
    pen_h=h; pen_v=v;
}
void qd_line(int dh, int dv){ qd_line_to(pen_h+dh, pen_v+dv); }

void qd_fill_rect(const Rect *r, int black){
    for(int y=r->top;y<r->bottom;y++) for(int x=r->left;x<r->right;x++) put(x,y,black);
}
void qd_paint_rect(const Rect *r){ qd_fill_rect(r, pen_black); }
void qd_erase_rect(const Rect *r){ qd_fill_rect(r, 0); }
void qd_invert_rect(const Rect *r){
    for(int y=r->top;y<r->bottom;y++) for(int x=r->left;x<r->right;x++) put(x,y,!getpix(x,y));
}
void qd_frame_rect(const Rect *r){
    hspan(r->left,r->right-1,r->top,pen_black); hspan(r->left,r->right-1,r->bottom-1,pen_black);
    for(int y=r->top;y<r->bottom;y++){ put(r->left,y,pen_black); put(r->right-1,y,pen_black); }
}
static void oval(const Rect *r, int fill, int black){
    int cx=(r->left+r->right)/2, cy=(r->top+r->bottom)/2;
    int a=(r->right-r->left)/2, b=(r->bottom-r->top)/2; if(a<=0||b<=0)return;
    for(int y=r->top;y<r->bottom;y++) for(int x=r->left;x<r->right;x++){
        double nx=(x-cx)/(double)a, ny=(y-cy)/(double)b, d=nx*nx+ny*ny;
        if(fill){ if(d<=1.0) put(x,y,black); }
        else    { if(d<=1.0 && d>0.72) put(x,y,black); }
    }
}
void qd_frame_oval(const Rect *r){ oval(r,0,pen_black); }
void qd_fill_oval(const Rect *r, int black){ oval(r,1,black); }

/* tiny 5x7 glyphs are overkill here; draw text as filled boxes so strings show */
/* Text. The pen sits on the baseline, as QuickDraw defines it, so a glyph
 * occupies the FONT_ROWS rows above it. Fixed pitch: five columns and one of
 * side bearing. Characters outside the font's range advance without drawing. */
#define GLYPH_W (FONT_COLS + 1)
void qd_draw_char(int c){
    c &= 0xFF;
    if(c >= FONT_FIRST && c <= FONT_LAST){
        const uint8_t *g = FONT5X7[c - FONT_FIRST];
        for(int col=0; col<FONT_COLS; col++)
            for(int row=0; row<FONT_ROWS; row++)
                if(g[col] & (1u << row)) put(pen_h+col, pen_v-FONT_ROWS+row, pen_black);
    }
    pen_h += GLYPH_W;
}
int qd_text_width(int len){ return len*GLYPH_W; }
void qd_draw_text(const uint8_t *p, int len){ for(int i=0;i<len;i++) qd_draw_char(p[i]); }

/* CopyBits: 1-bit source (row-padded to src_rowbytes) -> framebuffer, scaled by
 * simple nearest sampling from srcR to dstR. mode ignored except invert. */
void qd_copybits(const uint8_t *src, int src_rowbytes, int sw, int sh,
                 const Rect *srcR, const Rect *dstR, int mode){
    (void)sw;(void)sh;
    int sW=srcR->right-srcR->left, sH=srcR->bottom-srcR->top;
    int dW=dstR->right-dstR->left, dH=dstR->bottom-dstR->top;
    if(sW<=0||sH<=0||dW<=0||dH<=0) return;
    for(int dy=0;dy<dH;dy++){
        int sy=srcR->top + dy*sH/dH;
        for(int dx=0;dx<dW;dx++){
            int sx=srcR->left + dx*sW/dW;
            int bit=(src[sy*src_rowbytes + (sx>>3)] >> (7-(sx&7))) & 1; /* Mac: 1 bit = black */
            int px=dstR->left+dx, py=dstR->top+dy;
            if(mode==0x24 /*notSrcCopy*/) bit=!bit;
            put(px,py,bit);
        }
    }
}

/* ---- rect utilities ---- */
void rect_set(Rect *r,int l,int t,int rt,int b){ r->left=l;r->top=t;r->right=rt;r->bottom=b; }
void rect_offset(Rect *r,int dh,int dv){ r->left+=dh;r->right+=dh;r->top+=dv;r->bottom+=dv; }
void rect_inset(Rect *r,int dh,int dv){ r->left+=dh;r->right-=dh;r->top+=dv;r->bottom-=dv; }
int rect_sect(const Rect *a,const Rect *b,Rect *o){
    o->left=a->left>b->left?a->left:b->left; o->top=a->top>b->top?a->top:b->top;
    o->right=a->right<b->right?a->right:b->right; o->bottom=a->bottom<b->bottom?a->bottom:b->bottom;
    return o->right>o->left && o->bottom>o->top;
}
int rect_union(const Rect *a,const Rect *b,Rect *o){
    o->left=a->left<b->left?a->left:b->left; o->top=a->top<b->top?a->top:b->top;
    o->right=a->right>b->right?a->right:b->right; o->bottom=a->bottom>b->bottom?a->bottom:b->bottom;
    return 1;
}
int pt_in_rect(int h,int v,const Rect *r){ return h>=r->left&&h<r->right&&v>=r->top&&v<r->bottom; }

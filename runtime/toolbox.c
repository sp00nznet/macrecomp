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
#include <stdlib.h>

/* ---- Pascal stack helpers ---- */
static uint16_t pop16(void){ uint16_t v=(uint16_t)m68k_r16(SP); SP+=2; return v; }
static uint32_t pop32(void){ uint32_t v=m68k_r32(SP); SP+=4; return v; }
/* Pascal function results. The caller reserved the result slot *below* the
 * arguments, so once every argument is popped SP points straight at it.
 * Write there -- pushing would leave SP two bytes short and put the result
 * where nothing reads it. */
static void ret16(uint16_t v){ m68k_w16(SP,v); }
static void ret32(uint32_t v){ m68k_w32(SP,v); }
/* A Point argument is passed by value in a long: v in the high word, h in the low. */
static void pt_unpack(uint32_t v, int *h, int *vv){ *vv=(int16_t)(v>>16); *h=(int16_t)(v&0xFFFF); }

static Rect rd_rect(uint32_t p){ Rect r; r.top=(int16_t)m68k_r16(p); r.left=(int16_t)m68k_r16(p+2);
    r.bottom=(int16_t)m68k_r16(p+4); r.right=(int16_t)m68k_r16(p+6); return r; }
static void wr_rect(uint32_t p,const Rect*r){ m68k_w16(p,r->top); m68k_w16(p+2,r->left);
    m68k_w16(p+4,r->bottom); m68k_w16(p+6,r->right); }

/* The baseAddr that marks "the screen" (qd_fb) rather than an offscreen buffer
 * in M.mem. This used to be the constant 1, which worked only as long as nobody
 * looked at it: a title that keeps its own copy of the screen base and asserts
 * its port still points there compares a real pointer against 1 and concludes
 * the port has been redirected. HyperCard does exactly that and stops with
 * "Unexpected error 123452". So the screen gets a genuine block of guest memory,
 * sized like a real 1-bit screen; drawing still goes to qd_fb, the block exists
 * so that the address is a real one and comparisons against it hold. */
static uint32_t heap_alloc(uint32_t sz);
static uint32_t g_screen_base;
static uint32_t screen_base(void){
    if(!g_screen_base) g_screen_base = heap_alloc((uint32_t)QD_H * (QD_W/8));
    return g_screen_base;
}
static uint32_t g_front_win;   /* the one card window, for FindWindow */
static int g_update_pending;   /* an updateEvt the app has not been given yet */
static int g_peek_valid, g_peek_what, g_peek_msg, g_peek_h, g_peek_v;

/* WindowRecord past the 108-byte GrafPort: windowKind(108,2), visible(110,1),
 * hilited(111,1), goAwayFlag(112,1), spareFlag(113,1), strucRgn(114,4),
 * contRgn(118,4), updateRgn(122,4). */
#define WR_UPDATERGN 122
static uint32_t rgn_alloc(void);
static Rect rd_rect(uint32_t p);
static void rgn_put(uint32_t h, const Rect *r);
static Rect rgn_get(uint32_t h);
static int rect_empty(const Rect *r);
/* An update event is only half the story: having been told to repaint, a Mac
 * application asks EmptyRgn(theWindow->updateRgn) whether there is anything to
 * repaint, and a window with no update region at all answers "no" and draws
 * nothing. So the region has to exist and has to say what is dirty. */
/* GrafPort: device(0,2), portBits(2,14), portRect(16,8), visRgn(24,4),
 * clipRgn(28,4). A port whose visRgn is null reads back as an empty rect, and a
 * title that intersects against it to decide what to draw concludes nothing is
 * visible and draws nothing. Both regions have to exist and cover the port. */
static void port_regions(uint32_t p, const Rect *r){
    if(!p) return;
    for(int off = 24; off <= 28; off += 4){
        uint32_t rgn = m68k_r32(p + off);
        if(!rgn){ rgn = rgn_alloc(); if(!rgn) continue; m68k_w32(p + off, rgn); }
        rgn_put(rgn, r);
    }
}
/* Every window, not just the front one. A title can have several open and
 * paints each from its own update event; tracking one meant only one of
 * HyperCard's four was ever told to repaint -- and it was not the card. */
#define MAX_WINS 32
static uint32_t g_wins[MAX_WINS]; static int g_nwins;
static int g_win_pending[MAX_WINS];
static int win_index(uint32_t w){
    for(int i=0;i<g_nwins;i++) if(g_wins[i]==w) return i;
    if(g_nwins < MAX_WINS){ g_wins[g_nwins]=w; g_win_pending[g_nwins]=0; return g_nwins++; }
    return -1;
}
static void win_dirty(uint32_t w, int dirty){
    if(!w) return;
    uint32_t rgn = m68k_r32(w + WR_UPDATERGN);
    if(!rgn){ rgn = rgn_alloc(); if(!rgn) return; m68k_w32(w + WR_UPDATERGN, rgn); }
    int i = win_index(w);
    if(dirty){ Rect r = rd_rect(w + 16); rgn_put(rgn, &r); if(i>=0) g_win_pending[i]=1; }
    else { Rect z = {0,0,0,0}; rgn_put(rgn, &z); if(i>=0) g_win_pending[i]=0; }
}
static uint32_t g_cur_port = 0;             /* current GrafPort (for GetPort) */
/* BitMap layout: baseAddr(4), rowBytes(2), bounds Rect(8: top,left,bottom,right) */
static void bitmap_screen(uint32_t bm){ m68k_w32(bm,screen_base()); m68k_w16(bm+4,QD_W/8);
    Rect s={0,0,QD_H,QD_W}; wr_rect(bm+6,&s); }
static void set_target_from_bitmap(uint32_t bm){
    uint32_t base=m68k_r32(bm); int rb=(int)(m68k_r16(bm+4)&0x3FFF);
    int bt=(int16_t)m68k_r16(bm+6),  bl=(int16_t)m68k_r16(bm+8);
    int bb=(int16_t)m68k_r16(bm+10), br=(int16_t)m68k_r16(bm+12);
    qd_set_port(base==screen_base() || base==0, base, rb, bl, bt, br, bb);
}

/* ---- bump heap inside M.mem (NewPtr/NewHandle) ---- */
static uint32_t heap_ptr = 0, heap_end = 0;
static void heap_init(void){
    heap_ptr = 0x00800000; heap_end = 0x01E00000;        /* 8..30 MB */
    if(M.memsize < heap_end){                            /* smaller guest: top half */
        heap_ptr = M.memsize/2; heap_end = M.memsize;
    }
}
/* Bounds-checked against M.memsize as well as the heap end: the zero-fill below
 * touches M.mem directly, so an out-of-range block would run off the buffer
 * rather than being dropped the way m68k_w8 would drop it. */
/* What is left, which is what FreeMem and friends should be reporting. */
static uint32_t heap_free(void){
    uint32_t top = heap_end < M.memsize ? heap_end : M.memsize;
    return heap_ptr < top ? top - heap_ptr : 0;
}
static uint32_t heap_alloc(uint32_t sz){
    sz = (sz+3)&~3u;
    if(!sz || sz > heap_end || heap_ptr > heap_end - sz || heap_ptr + sz > M.memsize){
        /* Say so once. A title that cannot allocate puts up "out of memory"
         * and stops, and without this there is nothing to connect that dialog
         * to the heap actually running dry. */
        static int said = 0;
        if(!said++) fprintf(stderr, "m68k: heap exhausted asking for %u bytes "
                                    "(%u of %u used)\n",
                            sz, heap_ptr - 0x00800000u, heap_end - 0x00800000u);
        return 0;
    }
    uint32_t p = heap_ptr; heap_ptr += sz;
    memset(M.mem + p, 0, sz);
    return p;
}

/* Handle sizes are recorded next to the handle (see hsz_set below); resources
 * need it too, so declare it here. */
static void hsz_set(uint32_t h, uint32_t sz);

/* ---- Resource Manager: serve the app's own extracted resources ---- */
typedef struct { int file; char type[6]; int id; const uint8_t *data; int len; uint32_t handle; } Res;
static Res g_res[6000]; static int g_nres;
static int g_curres = 1;                  /* 1 = the application's own fork */

void res_add_file(int refnum, const char *type, int id, const uint8_t *data, int len){
    if(g_nres>=(int)(sizeof g_res/sizeof*g_res)) return;
    Res *r=&g_res[g_nres++]; int i=0;
    for(;i<5&&type[i];i++) r->type[i]=type[i];
    while(i>0&&r->type[i-1]==' ') i--;           /* strip trailing spaces */
    r->type[i]=0; r->file=refnum; r->id=id; r->data=data; r->len=len; r->handle=0;
}
void res_add(const char *type, int id, const uint8_t *data, int len){
    res_add_file(1, type, id, data, len);
}
void res_use_file(int refnum){ if(refnum) g_curres = refnum; }
int  res_cur_file(void){ return g_curres; }
static void type4(uint32_t t, char *out){ /* 'PICT' long -> stripped string */
    out[0]=t>>24; out[1]=t>>16; out[2]=t>>8; out[3]=t; out[4]=0;
    int n=4; while(n>0&&out[n-1]==' ') out[--n]=0;
}
/* Search the current resource file first, then the application's own fork --
 * the Resource Manager's chain, shortened to the two links that exist here. A
 * stack's own icons and scripts must win over HyperCard's. */
static void mr_text_probe(const char *tag);
static uint32_t res_get(uint32_t typelong, int id){
    char want[5]; type4(typelong,want);
    /* HyperTalk syntax errors are STR# 1002. Probe here rather than at the
     * ParamText that finally shows the text: this is ~8 frames closer to the
     * parser, where its cursor is still on the stack. */
    if(getenv("MRPARSE") && typelong==0x53545223u && id==1002) mr_text_probe("STR#1002");
    for(int pass=0; pass<2; pass++){
      int wantfile = pass==0 ? g_curres : 1;
      if(pass==1 && g_curres==1) break;
      for(int i=0;i<g_nres;i++){
        if(g_res[i].file==wantfile && g_res[i].id==id && strcmp(g_res[i].type,want)==0){
            Res *r=&g_res[i];
            if(!r->handle){                       /* lazy: copy into M.mem, make a handle */
                uint32_t p=heap_alloc(r->len); for(int k=0;k<r->len;k++) M.mem[p+k]=r->data[k];
                uint32_t h=heap_alloc(4); m68k_w32(h,p); r->handle=h;
                /* A resource handle is a handle like any other: GetHandleSize
                 * must answer for it. Without this it reports 0, and a caller
                 * that asks how big a resource is concludes it is empty. */
                hsz_set(h, (uint32_t)r->len);
            }
            if(getenv("MRTRACE")) fprintf(stderr, "  res '%s' %d (file %d) -> %06x\n",
                                          want, id, wantfile, r->handle);
            return r->handle;
        }
      }
    }
    /* A miss is the interesting case: a title that cannot find a resource it
     * requires usually quits rather than complains, so this is often the last
     * useful thing in a log. */
    if(getenv("MRTRACE")) fprintf(stderr, "  res '%s' %d -> MISSING\n", want, id);
    return 0;
}

/* ---- trap normalization: strip auto-pop / flag bits ---- */
/* Toolbox traps ($A800-$ABFF) carry a 10-bit trap number. OS traps carry a
 * 9-bit number with the flag bits above it -- bit 9 is "clear", bit 10 "sys" --
 * so NewHandle is $A122 and NewHandleClear $A322. Masking OS traps with $FF
 * folds $A122 onto $A022, which is not a trap at all. */
static uint16_t norm(uint16_t w){ return (w&0x0800)?(0xA800|(w&0x03FF)):(0xA000|(w&0x01FF)); }

/* ---- regions ----
 * A Region is rgnSize(2) + rgnBBox(8). We store rectangular regions only:
 * rgnSize stays 10 and there is never any scanline data.
 * ponytail: a non-rectangular region degrades to its bounding box. Real region
 * algebra (inversion-point encoding) only matters for a title that clips to a
 * non-rectangular shape; write it when one turns up. */
static int rect_empty(const Rect *r){ return r->right<=r->left || r->bottom<=r->top; }
static int rect_covers(const Rect *a, const Rect *b){   /* a fully contains b */
    return !rect_empty(b) && a->left<=b->left && a->top<=b->top
        && a->right>=b->right && a->bottom>=b->bottom;
}
static uint32_t rgn_alloc(void){
    uint32_t rp=heap_alloc(10), h=heap_alloc(4);
    if(rp){ Rect z={0,0,0,0}; m68k_w16(rp,10); wr_rect(rp+2,&z); }
    if(h) m68k_w32(h,rp);
    return h;
}
static Rect rgn_get(uint32_t h){
    Rect z={0,0,0,0}; if(!h) return z;
    uint32_t rp=m68k_r32(h); if(!rp) return z;
    return rd_rect(rp+2);
}
static void rgn_put(uint32_t h, const Rect *r){
    if(!h) return; uint32_t rp=m68k_r32(h); if(!rp) return;
    m68k_w16(rp,10); wr_rect(rp+2,r);
}

/* ---- handle sizes ----
 * The bump heap never frees, so a handle's size is simply recorded next to it.
 * ponytail: linear scan. Swap for a hash if a title allocates thousands. */
static struct { uint32_t h, size; } g_hsz[4096];
static int g_nhsz;
static void hsz_set(uint32_t h, uint32_t sz){
    for(int i=0;i<g_nhsz;i++) if(g_hsz[i].h==h){ g_hsz[i].size=sz; return; }
    if(g_nhsz<(int)(sizeof g_hsz/sizeof *g_hsz)){ g_hsz[g_nhsz].h=h; g_hsz[g_nhsz++].size=sz; }
    else { static int said=0; if(!said++) fprintf(stderr,"m68k: handle size table full at %d; "
        "sizes for later handles are lost\n", g_nhsz); }
}
static uint32_t hsz_get(uint32_t h){
    for(int i=0;i<g_nhsz;i++) if(g_hsz[i].h==h) return g_hsz[i].size;
    return 0;
}
/* Zero is a real size, so "not in the table" has to be asked separately --
 * otherwise an unrecorded handle looks empty and gets treated as one. */
static int hsz_known(uint32_t h){
    for(int i=0;i<g_nhsz;i++) if(g_hsz[i].h==h) return 1;
    return 0;
}

/* ---- dialogs ---- */
#define DLG_ARENA (64u * 304u)          /* MAX_ITEMS * ITEM_SLOT in dialog.c */
static uint32_t g_front_dlg = 0;
static uint32_t make_dialog(uint32_t dstor, uint32_t ditl, const Rect *bounds){
    uint32_t d = dstor ? dstor : heap_alloc(256);
    if(!d) return 0;
    bitmap_screen(d+2);                                  /* GrafPort.portBits */
    Rect pr; rect_set(&pr, 0, 0, bounds->bottom-bounds->top, bounds->right-bounds->left);
    wr_rect(d+16, &pr);                                  /* GrafPort.portRect */
    uint32_t arena = heap_alloc(DLG_ARENA);
    dlg_new(d, ditl, arena, arena + DLG_ARENA);
    dlg_set_bounds(d, bounds);        /* DITL boxes are window-local */
    g_front_dlg = d; g_cur_port = d;
    return d;
}

static uint32_t g_rand = 0x12345678u;    /* Random(): deterministic by design */

/* ---- low-memory globals ----
 * Classic Mac code reads these addresses directly rather than through a trap,
 * so the HAL has to populate them or the app sees a machine made of zeroes.
 * HyperCard's "requires more recent ROMs" check is exactly this: it reads
 * ROM85 and the ROM version word at ROMBase+8. */
#define LM_MEMTOP    0x0108
#define LM_TICKS     0x016A
#define LM_ROM85     0x028E
#define LM_ROMBASE   0x02AE
#define LM_SCRNBASE  0x0824
#define LM_CURRENTA5 0x0904

static void lowmem_init(void){
    uint32_t rom = heap_alloc(256);          /* a stand-in ROM header */
    m68k_w16(LM_ROM85, 0x007F);              /* 128K ROM or later: high bit clear */
    m68k_w32(LM_ROMBASE, rom);
    m68k_w16(rom + 8, 0x0276);               /* ROM version word: Mac SE */
    m68k_w32(LM_MEMTOP, M.memsize);
    m68k_w32(LM_SCRNBASE, screen_base());
    m68k_w32(LM_TICKS, 0);
    m68k_w32(LM_CURRENTA5, M.a[5]);
}

static int g_inited = 0;
uint32_t mr_alloc(uint32_t n){ return heap_alloc(n); }

/* GetAppParms reports the application's own name. MRAPP names it; a title that
 * looks at it mostly wants something non-empty.
 *
 * Not built: the Finder startup handshake (an AppParmHandle block that
 * CountAppFiles/GetAppFiles walk to learn which document to open). It was
 * written and then removed unused -- HyperCard 1.2.2 never calls GetAppParms,
 * so it was 40 lines answering a question nothing asked. Add it when a title
 * actually calls CountAppFiles; the block is message(2), count(2), then per
 * file vRefNum(2), type(4), versNum(2), Str255 padded even (IM II-57). */
static const char *app_name(void){
    const char *e = getenv("MRAPP");
    return (e && *e) ? e : "Application";
}

void toolbox_init(void){ qd_init(); heap_init(); lowmem_init(); g_inited=1;
    { const char *e = getenv("MRWATCHADDR");
      g_watch_addr = e ? (uint32_t)strtoul(e,0,16) : MR_WATCH_OFF; } }

/* ---- PICT v1 decode + blit (DrawPicture) ---- */
static uint32_t unpackbits_row(uint32_t p, uint8_t *out, int rowbytes){
    int n=0; while(n<rowbytes){ int8_t c=(int8_t)m68k_r8(p++);
        if(c>=0){ int cnt=c+1; for(int i=0;i<cnt&&n<rowbytes;i++) out[n++]=(uint8_t)m68k_r8(p++); }
        else { int cnt=257-(uint8_t)(c&0xff); uint8_t v=(uint8_t)m68k_r8(p++); for(int i=0;i<cnt&&n<rowbytes;i++) out[n++]=v; } }
    return p;
}
static void draw_pict(uint32_t pic, Rect dst){
    uint32_t o=pic+2+8;                 /* skip picSize + frame rect */
    for(int guard=0; guard<4096; guard++){
        uint8_t op=(uint8_t)m68k_r8(o++);
        if(op==0xFF) break;
        if(op==0x90||op==0x98){          /* BitsRect / PackBitsRect */
            int rowbytes=(int)m68k_r16(o); o+=2;
            int t=(int16_t)m68k_r16(o), l=(int16_t)m68k_r16(o+2),
                b=(int16_t)m68k_r16(o+4), r=(int16_t)m68k_r16(o+6); o+=8;
            o+=8+8+2;                    /* srcRect, dstRect, mode */
            int w=r-l, h=b-t; if(rowbytes&0x8000||w<=0||h<=0||rowbytes>512) return;
            static uint8_t row[512];
            int dw=dst.right-dst.left, dh=dst.bottom-dst.top;
            if(dw<=0)dw=w; if(dh<=0)dh=h;
            for(int y=0;y<h;y++){
                /* The row header holds the packed byte count. unpackbits_row
                 * stops on output length instead and returns the new offset,
                 * so the count is skipped, not read.
                 * ponytail: trusting the output length; bound the row by the
                 * header count if a malformed PICT ever over-reads. */
                if(op==0x98){ o += rowbytes>250?2:1;
                              uint32_t np=unpackbits_row(o,row,rowbytes); o=np; }
                else { for(int i=0;i<rowbytes;i++) row[i]=(uint8_t)m68k_r8(o+i); o+=rowbytes; }
                int py=dst.top + y*dh/h;
                for(int x=0;x<w;x++){ int bit=(row[x>>3]>>(7-(x&7)))&1;
                    int px=dst.left + x*dw/w; if(px>=0&&px<QD_W&&py>=0&&py<QD_H) qd_fb[py][px]=bit; }
            }
            return;                      /* one bitmap is the picture */
        }
        else if(op==0x01){ int rs=(int)m68k_r16(o); o+=rs; }       /* clipRgn */
        else if(op==0xA1){ o+=2; int sz=(int)m68k_r16(o); o+=2+sz; }/* long comment */
        else if(op==0xA0){ o+=2; }                                  /* short comment */
        else if(op==0x11){ o+=1; }                                  /* version */
        else if(op==0x00){ }                                        /* nop */
        else if(op<0x30){ o+=8; }        /* rough skip for state/line ops */
        else if(op<0x60){ o+= (op&0x08)?0:8; }  /* rect/oval ops: 8 or 0 (reuse) */
        else return;                     /* unknown: stop */
    }
}

static void logtrap(uint16_t w){
    static uint16_t seen[512]; static int ns=0;
    for(int i=0;i<ns;i++) if(seen[i]==w) return;
    if(ns<512) seen[ns++]=w;
    fprintf(stderr,"trap $%04X unimplemented\n", w);
}

/* Report every register and stack slot that points at a run of script text.
 * A HyperTalk parse error names a token but not a place; the parser cursor is
 * somewhere in the frame, and printing the line it sits on turns the message
 * into a source location. Set MRPARSE to switch it on. */
static void mr_text_probe(const char *tag){
    fprintf(stderr,"[parse:%s] scan sp=%06x a6=%06x d0=%08x a0=%06x\n",
            tag, SP, M.a[6], M.d[0], M.a[0]);
    fprintf(stderr,"[parse:%s] call chain:", tag);
    for(int f=g_shadow_sp-1; f>=0 && f>g_shadow_sp-24; f--)
        fprintf(stderr," %06x", (unsigned)g_shadow[f]);
    fprintf(stderr,"\n");
    for(int i=0;i<16+4096 && (i<16 || SP+4u*(i-16)<0x3F0000u);i++){
        uint32_t v = i<8 ? M.d[i] : i<16 ? M.a[i-8] : m68k_r32(SP+4u*(i-16));
        if(v<64 || v+64>=M.memsize) continue;
        int printable=0;
        for(int k=-32;k<32;k++){ uint8_t c=M.mem[v+k];
            if((c>=0x20&&c<0x7f)||c==0x0D||c==0x09) printable++; }
        if(printable<62) continue;
        uint32_t ls=v, le=v;
        while(ls>0 && M.mem[ls-1]!=0x0D && v-ls<100) ls--;
        while(le+1<M.memsize && M.mem[le]!=0x0D && M.mem[le] && le-v<100) le++;
        fprintf(stderr,"[parse:%s] ",tag);
        if(i<16) fprintf(stderr,"%s%d",i<8?"d":"a",i<8?i:i-8);
        else     fprintf(stderr,"%d(sp)",4*(i-16));
        fprintf(stderr," = %06x col %u: \"", v, (unsigned)(v-ls));
        for(uint32_t q=ls;q<le;q++) fputc(M.mem[q],stderr);
        fprintf(stderr,"\"\n"); }
    /* A script cursor is usually a handle plus an offset, not a bare pointer,
     * so also follow one level of indirection: slot -> master pointer -> text. */
    for(int i=0;i<16+4096 && (i<16 || SP+4u*(i-16)<0x3F0000u);i++){
        uint32_t hv = i<8 ? M.d[i] : i<16 ? M.a[i-8] : m68k_r32(SP+4u*(i-16));
        if(hv<64 || hv+4>=M.memsize) continue;
        uint32_t v = m68k_r32(hv);
        if(v<64 || v+64>=M.memsize) continue;
        int printable=0;
        for(int k=0;k<64;k++){ uint8_t c=M.mem[v+k];
            if((c>=0x20&&c<0x7f)||c==0x0D||c==0x09) printable++; }
        if(printable<60) continue;
        fprintf(stderr,"[parse:%s] *",tag);
        if(i<16) fprintf(stderr,"%s%d",i<8?"d":"a",i<8?i:i-8);
        else     fprintf(stderr,"%d(sp)",4*(i-16));
        fprintf(stderr," -> %06x: \"", v);
        for(int q=0;q<64;q++){ uint8_t c=M.mem[v+q]; fputc(c==0x0D?'|':c, stderr); }
        fprintf(stderr,"\"\n"); }
}

void m68k_trap(uint16_t raw){
    uint16_t w = norm(raw);
    if(getenv("MRPROBE") && w==0xA9C8){
        uint32_t a5=M.a[5];
        fprintf(stderr,"  PROBE WTLK globals: %08x %08x %08x %08x  (a5=%08x)\n",
            m68k_r32(a5-0x2bb4), m68k_r32(a5-0x2ba8), m68k_r32(a5-0x2bac), m68k_r32(a5-0x2bb0), a5); }
    if(getenv("MRTRACE")){ static long n=0;
        fprintf(stderr,"[%5ld] $%04X  from %06x  depth %d\n", n++, w, g_last_call, g_shadow_sp);
        /* MRSTACK: the whole shadow stack per trap. The last trap before a hang
         * names every frame the loop could be in; a single caller address does
         * not, because the loop is usually in an outer frame. */
        if(getenv("MRSTACK")){ int d=g_shadow_sp<512?g_shadow_sp:512;
            for(int i=0;i<d;i++) fprintf(stderr,"        [%d] %06x\n", i, g_shadow[i]); } }
    { const char *mp=getenv("MRPRESENT"); if(mp){ int iv=atoi(mp); if(iv<=0)iv=300;
        static long p=0; if(++p%iv==0) plat_present(); } }
    switch(w){
    /* ---- init (mostly no-ops for us) ---- */
    /* InitGraf(globalsPtr). The argument points at the LAST field of QDGlobals
     * (thePort), so every other field sits at a fixed negative offset from it.
     * Discarding the pointer, as this used to, leaves screenBits, the five
     * standard patterns, the arrow cursor and randSeed as whatever happened to
     * be in memory -- and a title that reads qd.screenBits.baseAddr to learn
     * where the screen is then believes the screen is somewhere else. */
    case 0xA86E: { /*InitGraf*/
        uint32_t gp = pop32();
        if(getenv("MRTRACE")) fprintf(stderr,
            "  InitGraf globals=%06x (a5%+d), screenBits at a5%+d\n",
            gp, (int)(gp-M.a[5]), (int)(gp-122-M.a[5]));
        if(gp >= 128){
            m68k_w32(gp, 0);                      /* thePort: none yet */
            bitmap_screen(gp - 122);              /* screenBits */
            /* white, black, gray, ltGray, dkGray -- 8 bytes each, walking down */
            static const uint8_t pats[5][8] = {
                {0,0,0,0,0,0,0,0},
                {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
                {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55},
                {0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22},
                {0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD},
            };
            for(int i=0;i<5;i++) for(int k=0;k<8;k++)
                m68k_w8(gp - 8u*(uint32_t)(i+1) + (uint32_t)k, pats[i][k]);
            m68k_w32(gp - 126, 1);                /* randSeed */
        } } break;
    case 0xA8FE: /*InitFonts*/ case 0xA912: /*InitWindows*/ case 0xA930: /*InitMenus*/
    case 0xA9CC: /*TEInit*/    case 0xA063: /*MaxApplZone*/ case 0xA850: /*InitCursor*/
    case 0xA036: /*MoreMasters*/ break;
    case 0xA97B: /*InitDialogs*/ (void)pop32(); break;   /* arg: resumeProc */

    /* ---- TextEdit (textedit.c) ----
     * Pascal order: the last argument pushed is the first popped, and the
     * result slot the caller reserved sits at SP once they all are. */
    case 0xA9D2: { /*TENew(destRect,viewRect): TEHandle*/
        uint32_t view=pop32(), dest=pop32(); ret32(te_new(dest,view)); } break;
    case 0xA9CD: /*TEDispose*/ te_dispose(pop32()); break;
    case 0xA9CF: { /*TESetText(text,length,hTE)*/
        uint32_t h=pop32(); uint32_t n=pop32(); uint32_t t=pop32();
        te_set_text(h,t,(int)n); } break;
    case 0xA9CB: { /*TEGetText(hTE): CharsHandle*/
        uint32_t h=pop32(); ret32(te_get_text(h)); } break;
    case 0xA9D0: /*TECalText*/ te_calc(pop32()); break;
    case 0xA9D3: { /*TEUpdate(rUpdate,hTE)*/
        uint32_t h=pop32(); (void)pop32(); te_update(h); } break;
    case 0xA9D8: /*TEActivate*/   te_activate(pop32(),1); break;
    case 0xA9D9: /*TEDeactivate*/ te_activate(pop32(),0); break;
    case 0xA9DA: /*TEIdle*/       te_idle(pop32()); break;
    case 0xA9D1: { /*TESetSelect(selStart,selEnd,hTE)*/
        uint32_t h=pop32(); uint32_t b=pop32(); uint32_t a=pop32();
        te_set_select(h,(int)a,(int)b); } break;
    case 0xA9D4: { /*TEClick(pt,extend,hTE)*/
        uint32_t h=pop32(); int ext=pop16()!=0; uint32_t pt=pop32();
        int ph,pv; pt_unpack(pt,&ph,&pv); te_click(h,ph,pv,ext); } break;
    case 0xA9DC: { /*TEKey(key,hTE)*/
        uint32_t h=pop32(); int c=pop16()&0xFF; te_key(h,c); } break;
    case 0xA9DE: { /*TEInsert(text,length,hTE)*/
        uint32_t h=pop32(); uint32_t n=pop32(); uint32_t t=pop32();
        te_insert(h,t,(int)n); } break;
    case 0xA9D7: /*TEDelete*/ te_delete(pop32()); break;
    case 0xA9D6: /*TECut*/    te_cut(pop32());    break;
    case 0xA9D5: /*TECopy*/   te_copy(pop32());   break;
    case 0xA9DB: /*TEPaste*/  te_paste(pop32());  break;
    case 0xA9CE: { /*TETextBox(text,length,box,just)*/
        (void)pop16(); uint32_t bx=pop32(); uint32_t n=pop32(); uint32_t t=pop32();
        Rect b=rd_rect(bx); te_text_box(t,(int)n,&b,0); } break;
    /* Scrolling is not modelled: destRect is where the text is drawn and the
     * view clips it, so a scroll that moved destRect would need the caret and
     * click mapping to follow. Accept and reflow instead of drifting.
     * ponytail: no scroll offset; add one when a title scrolls a real field. */
    case 0xA9DD: /*TEScroll*/ case 0xA812: { /*TEPinScroll(dh,dv,hTE)*/
        uint32_t h=pop32(); (void)pop16(); (void)pop16(); te_calc(h); } break;
    case 0xA813: { /*TEAutoView(auto,hTE)*/ (void)pop32(); (void)pop16(); } break;
    case 0xA811: /*TESelView*/ (void)pop32(); break;
    case 0xA032: /*FlushEvents*/ (void)pop32(); break;
    case 0xA9F4: /*ExitToShell*/ fprintf(stderr,"[ExitToShell]\n"); plat_present(); exit(0);

    /* ---- QuickDraw: pen & text state ---- */
    case 0xA873: /*SetPort*/ { uint32_t p=pop32(); g_cur_port=p; if(p) set_target_from_bitmap(p+2); } break;
    case 0xA874: /*GetPort*/ { uint32_t pp=pop32(); if(pp) m68k_w32(pp,g_cur_port); } break;
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
        ret16(nz?1:0); } break;
    case 0xA8AB: /*UnionRect*/ { uint32_t dst=pop32(),b=pop32(),a=pop32();
        Rect ra=rd_rect(a),rb=rd_rect(b),o; rect_union(&ra,&rb,&o); wr_rect(dst,&o); } break;
    case 0xA8AD: /*PtInRect*/ { uint32_t rp=pop32(); uint32_t pt=pop32();
        int h,v; pt_unpack(pt,&h,&v); Rect rr=rd_rect(rp);
        ret16(pt_in_rect(h,v,&rr)?1:0); } break;

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
    case 0xA8B8: /*EraseOval*/ { Rect r=rd_rect(pop32()); qd_fill_oval(&r,0); } break;
    case 0xA8B9: /*InvertOval*/ { Rect r=rd_rect(pop32()); qd_fill_oval(&r,1); } break;
    case 0xA8BB: /*FillOval*/  { (void)pop32(); Rect r=rd_rect(pop32()); qd_fill_oval(&r,1); } break;
    case 0xA87B: /*ClipRect*/  { Rect r=rd_rect(pop32()); qd_set_clip(&r);
        if(getenv("MRGFX")) fprintf(stderr,"[gfx] ClipRect %d,%d,%d,%d\n",r.top,r.left,r.bottom,r.right); } break;
    case 0xA884: /*DrawString*/{ uint32_t s=pop32(); int len=m68k_r8(s); uint8_t buf[256];
        for(int i=0;i<len;i++) buf[i]=(uint8_t)m68k_r8(s+1+i); qd_draw_text(buf,len); } break;
    case 0xA883: /*DrawChar*/  { int c=pop16();
        if(getenv("MRGFX")){ int ph,pv; qd_get_pen(&ph,&pv);
            fprintf(stderr,"[gfx] DrawChar '%c' at %d,%d\n", (c>=32&&c<127)?c:46, ph, pv); }
        qd_draw_char(c); } break;

    /* ---- events (thin) ---- */
    case 0xA975: /*TickCount*/ ret32(plat_ticks()); break;
    case 0xA972: /*GetMouse*/ { uint32_t pt=pop32(); int h,v; plat_get_mouse(&h,&v);
        m68k_w16(pt,v); m68k_w16(pt+2,h); } break;
    case 0xA974: /*Button*/ ret16(plat_button()?1:0); break;
    case 0xA973: /*StillDown*/ ret16(plat_button()?1:0); break;
    /* EventAvail reports the next event and *leaves it in the queue*;
     * GetNextEvent removes it. Sharing one implementation meant every peek ate
     * an event, so a title that polls with EventAvail and then fetches with
     * GetNextEvent -- which is the ordinary idiom, and what HyperCard does,
     * five times more often than it fetches -- lost nearly all of them,
     * mouse clicks included. One slot of pushback is enough to tell them
     * apart. */
    case 0xA970: /*GetNextEvent*/ case 0xA971: /*EventAvail*/ {
        plat_present();                       /* reaching the event loop == booted */
        if(plat_quit_requested()) exit(0);
        int peek = (w == 0xA971);
        uint32_t evp=pop32(); (void)pop16(); int what=0,msg=0,h=0,v=0;
        plat_pump();
        int got;
        if(g_peek_valid){
            what=g_peek_what; msg=g_peek_msg; h=g_peek_h; v=g_peek_v; got=1;
            if(!peek) g_peek_valid=0;
        } else {
            got = plat_next_event(&what,&msg,&h,&v);
            if(got && peek){ g_peek_valid=1; g_peek_what=what; g_peek_msg=msg;
                             g_peek_h=h; g_peek_v=v; }
        }
        /* A Mac application draws a window's contents only when handed an
         * updateEvt, and nothing here raises one: the window gets shown and is
         * then never painted, which looks exactly like a title that failed to
         * draw. Deliver it once per exposure -- clearing on delivery rather
         * than waiting for BeginUpdate means an app that never calls
         * BeginUpdate cannot spin on it. */
        if(!got){
            /* One update per dirty, visible window. The pending flag is the
             * gate rather than the region itself, so a title that ignores the
             * event is not handed it again for ever. */
            for(int i=0;i<g_nwins;i++){
                uint32_t wp = g_wins[i];
                if(!wp || !g_win_pending[i] || !m68k_r8(wp+110)) continue;
                what = 6 /*updateEvt*/; msg = (int)wp; got = 1;
                if(!peek) g_win_pending[i] = 0;
                if(getenv("MRTRACE")) fprintf(stderr,"  updateEvt -> window %06x%s\n",
                    (unsigned)wp, peek?" (peek)":"");
                break;
            }
        }
        if(evp){ m68k_w16(evp,what); m68k_w32(evp+2,msg); m68k_w32(evp+6,plat_ticks());
                 int mh,mv; plat_get_mouse(&mh,&mv); m68k_w16(evp+10,mv); m68k_w16(evp+12,mh);
                 m68k_w16(evp+14,0); }
        ret16(got?1:0); } break;

    /* ---- cursor / port (mostly no-ops; QuickDraw draws to one framebuffer) ---- */
    case 0xA852: /*HideCursor*/ case 0xA853: /*ShowCursor*/ case 0xA856: /*ObscureCursor*/
    case 0xA9B4: /*SystemTask*/ break;
    case 0xA86F: /*OpenPort*/ { uint32_t p=pop32(); if(p){ bitmap_screen(p+2);
        Rect s; rect_set(&s,0,0,QD_W,QD_H); wr_rect(p+16,&s); port_regions(p,&s);
        g_cur_port=p; qd_set_port(1,screen_base(),QD_W/8,0,0,QD_W,QD_H);} } break;
    case 0xA875: /*SetPortBits*/ { uint32_t bm=pop32();
        if(bm){
            /* SetPortBits *copies* the BitMap into thePort->portBits; it does
             * not merely redirect where drawing lands. Retargeting alone leaves
             * the port's own baseAddr in guest memory stale, and a title that
             * reads it back to check which buffer it is drawing into concludes
             * it is somewhere it is not. HyperCard asserts exactly that. */
            if(g_cur_port) for(int i=0;i<14;i++) m68k_w8(g_cur_port+2+i, m68k_r8(bm+i));
            set_target_from_bitmap(bm);
        }
        if(getenv("MRGFX")&&bm) fprintf(stderr,"[gfx] SetPortBits base=%x rb=%d\n",m68k_r32(bm),m68k_r16(bm+4)&0x3fff); } break;
    case 0xA9B8: /*GetPattern*/ { (void)pop16(); uint32_t h=heap_alloc(4),p=heap_alloc(8);
        for(int i=0;i<8;i++) M.mem[p+i]=0xFF; if(h)m68k_w32(h,p); ret32(h); } break;

    /* ---- Window Manager (thin: return a real WindowRecord so the game can draw) ---- */
    case 0xA913: /*NewWindow*/ {
        uint32_t refcon=pop32(); (void)pop16(); uint32_t behind=pop32(); (void)behind;
        (void)pop16(); int visible=(int16_t)pop16(); uint32_t title=pop32(); (void)title;
        uint32_t bounds=pop32(); uint32_t wstor=pop32();
        uint32_t w = wstor ? wstor : heap_alloc(256);
        Rect br = bounds?rd_rect(bounds):(Rect){0,0,QD_H,QD_W};
        Rect pr; rect_set(&pr,0,0,br.bottom-br.top,br.right-br.left);
        bitmap_screen(w+2);                 /* GrafPort.portBits -> the screen */
        wr_rect(w+16, &pr);                 /* GrafPort.portRect */
        m68k_w32(w+0xFC, refcon);           /* WindowRecord.refCon (approx offset) */
        /* WindowRecord past the 108-byte GrafPort: windowKind(2), visible(1),
         * hilited(1). A window whose visible byte is left at zero is one the
         * title will not draw into, however complete the port is. */
        m68k_w16(w+108, 8 /*userKind*/); m68k_w8(w+110, visible?1:0); m68k_w8(w+111, 1);
        port_regions(w, &pr);
        win_dirty(w, 1);
        g_front_win = w; g_update_pending = 1;
        m68k_w32(SP, w);                    /* Pascal result slot */
    } break;
    case 0xA9BD: /*GetNewWindow*/ {
        uint32_t behind=pop32(); (void)behind; uint32_t wstor=pop32(); (void)pop16();
        uint32_t w = wstor ? wstor : heap_alloc(256);
        Rect pr; rect_set(&pr,0,0,QD_H,QD_W); bitmap_screen(w+2); wr_rect(w+16,&pr);
        m68k_w16(w+108, 8 /*userKind*/); m68k_w8(w+110, 1); m68k_w8(w+111, 1);
        port_regions(w, &pr);
        win_dirty(w, 1);
        g_front_win = w; g_update_pending = 1;
        m68k_w32(SP, w);
    } break;
    case 0xA910: /*GetWMgrPort*/ { uint32_t pp=pop32(); if(pp)m68k_w32(pp,0); } break;
    case 0xA914: /*DisposeWindow*/ { uint32_t w=pop32(); if(w==g_front_win) g_front_win=0; } break;
    case 0xA916: /*HideWindow*/ case 0xA904: /*DrawGrowIcon*/ (void)pop32(); break;
    /* ValidRect/ValidRgn remove area from the update region -- that is the
     * whole point of them. A no-op leaves the window permanently dirty, and an
     * application that validates and then re-checks spins for ever. */
    case 0xA92A: /*ValidRect*/ case 0xA929: /*ValidRgn*/
        (void)pop32(); win_dirty(g_cur_port, 0); break;
    /* Anything that exposes window content owes the app an update event; there
     * is no real window server here to raise one. */
    /* Showing or selecting a window says which one is front far more reliably
     * than "the last one created" -- a title makes several windows and shows
     * the one it wants, so an update addressed to the newest can name a window
     * the title is not drawing into. */
    case 0xA91F: /*SelectWindow*/ case 0xA915: /*ShowWindow*/ {
        uint32_t w = pop32();
        if(getenv("MRGFX")) fprintf(stderr,"[gfx] %s %06x\n",
            w==0?"Show/Select NULL":(norm(raw)==0xA915?"ShowWindow":"SelectWindow"), (unsigned)w);
        if(w){ g_front_win = w; m68k_w8(w+110, 1); m68k_w8(w+111, 1); win_dirty(w, 1); }
        g_update_pending = 1; } break;
    /* Inval/Valid apply to the CURRENT PORT, not to whichever window is front:
     * validating one window was clearing another's pending update, so the card
     * window never got told to repaint. */
    case 0xA928: /*InvalRect*/  case 0xA927: /*InvalRgn*/
        (void)pop32(); win_dirty(g_cur_port, 1); break;
    /* BeginUpdate leaves the region set: the app is about to ask whether there
     * is anything to draw, and EndUpdate is where it stops being dirty. */
    case 0xA922: /*BeginUpdate*/ { uint32_t w = pop32();
        /* The real trap replaces the port's visRgn with what needs repainting,
         * which is how an app discovers there is anything to do. */
        if(w){ Rect u = rgn_get(m68k_r32(w + WR_UPDATERGN));
               if(rect_empty(&u)) u = rd_rect(w + 16);
               uint32_t vis = m68k_r32(w + 24);
               if(vis) rgn_put(vis, &u); }
        g_update_pending = 0; } break;
    case 0xA923: /*EndUpdate*/ { uint32_t w = pop32(); win_dirty(w, 0); } break;
    case 0xA924: /*FrontWindow*/ ret32(g_front_win); break;
    /* FindWindow is what turns a click into a destination. One full-screen card
     * window means the only distinction that matters is menu bar vs. content;
     * answering inDesk for everything, as an unimplemented trap effectively
     * does, drops every click and nothing can be navigated. */
    case 0xA92C: /*FindWindow*/ { uint32_t wp=pop32(), pt=pop32(); int h,v;
        pt_unpack(pt,&h,&v); (void)h;
        int part = v<20 ? 1/*inMenuBar*/ : g_front_win ? 3/*inContent*/ : 0/*inDesk*/;
        if(wp) m68k_w32(wp, part==3 ? g_front_win : 0);
        ret16((uint16_t)part); } break;
    case 0xA851: /*SetCursor*/ (void)pop32(); break;   /* nothing draws a cursor */
    case 0xA976: /*GetKeys*/ { uint32_t km=pop32();    /* no modifier is held */
        if(km) for(int i=0;i<16;i+=4) m68k_w32(km+i,0); } break;
    /* One full-screen port stands in for the window list, so a window's title
     * and position are accepted and discarded rather than refused: a title that
     * cannot set them has no way forward, and nothing here can show them. */
    case 0xA91A: /*SetWTitle*/ (void)pop32(); (void)pop32(); break;
    case 0xA91B: /*MoveWindow*/ (void)pop16(); (void)pop16(); (void)pop16(); (void)pop32(); break;
    case 0xA91D: /*SizeWindow*/ (void)pop16(); (void)pop16(); (void)pop16(); (void)pop32(); break;
    case 0xA8A6: /*EqualRect*/ { Rect b=rd_rect(pop32()), a=rd_rect(pop32());
        ret16((uint16_t)(a.top==b.top && a.left==b.left &&
                         a.bottom==b.bottom && a.right==b.right)); } break;
    case 0xA9B9: /*GetCursor*/ { (void)pop16();
        /* No cursor artwork is drawn, but the handle must be real: callers
         * dereference it and pass it to SetCursor. */
        uint32_t p2=heap_alloc(68), h=heap_alloc(4);
        if(h) m68k_w32(h,p2); m68k_w32(SP,h); } break;
    case 0xA8EA: /*SetStdProcs*/ (void)pop32(); break;   /* no custom drawing hooks */
    case 0xA8AE: /*EmptyRect*/ { Rect r=rd_rect(pop32());
        ret16((uint16_t)(r.right<=r.left || r.bottom<=r.top)); } break;
    case 0xA919: /*GetWTitle*/ { uint32_t nm=pop32(); (void)pop32();
        if(nm) m68k_w8(nm,0); } break;                  /* untitled: one port */
    case 0xA936: /*DeleteMenu*/ (void)pop16(); break;
    case 0xA807: /*SndNewChannel*/ (void)pop32(); (void)pop32(); (void)pop16();
        (void)pop32(); ret16((uint16_t)(-201)); break;  /* notEnoughHardwareErr */
    /* StripAddress is a **no-op here**, not a 24-bit mask. It exists because a
     * 24-bit machine kept flags in a pointer's top byte; masking on a 32-bit
     * clean address space would truncate every heap pointer above 16 MB, and
     * this runtime's heap starts at 8 MB and runs to 30 MB. The address is
     * already clean, so hand it straight back. */
    case 0xA055: /*StripAddress*/ break;
    /* Colour QuickDraw is not implemented, and a device list with no entries is
     * the honest answer -- a null main device says "monochrome" rather than
     * handing back something that cannot be walked. */
    case 0xAA29: /*GetDeviceList*/ case 0xAA2A: /*GetMainDevice*/
    case 0xAA2B: /*GetNextDevice*/ ret32(0); break;

    /* ---- Menu Manager (thin stubs; menu bar not drawn) ---- */
    case 0xA931: /*NewMenu*/ { uint32_t title=pop32(); (void)title; (void)pop16();
        m68k_w32(SP, heap_alloc(64)); } break;
    case 0xA935: /*InsertMenu*/ { (void)pop16(); (void)pop32(); } break;
    case 0xA934: /*ClearMenuBar*/ case 0xA937: /*DrawMenuBar*/ break;
    case 0xA933: /*AppendMenu*/ case 0xA94D: /*AppendResMenu*/ { (void)pop32(); (void)pop32(); } break;
    case 0xA939: /*EnableItem*/ case 0xA93A: /*DisableItem*/ { (void)pop16(); (void)pop32(); } break;
    case 0xA945: /*CheckItem*/ { (void)pop16(); (void)pop16(); (void)pop32(); } break;
    case 0xA938: /*HiliteMenu*/ case 0xA94C: /*FlashMenuBar*/ (void)pop16(); break;
    case 0xA93C: /*SetMenuBar*/ (void)pop32(); break;
    case 0xA93B: /*GetMenuBar*/ ret32(heap_alloc(4)); break;
    case 0xA948: /*CalcMenuSize*/ (void)pop32(); break;
    case 0xA950: /*CountMItems*/ { (void)pop32(); ret16(0); } break;
    case 0xADC0: /*GetNewMBar*/ { (void)pop16(); ret32(heap_alloc(4)); } break;
    case 0xA9C9: /*SysError*/ (void)pop16(); break;

    /* ---- DrawPicture: blit real PICT art ---- */
    case 0xA8F6: /*DrawPicture*/ { uint32_t rp=pop32(); uint32_t pich=pop32();
        uint32_t pic = pich ? m68k_r32(pich) : 0; Rect dst = rd_rect(rp);
        if(pic) draw_pict(pic, dst);
        if(getenv("MRSHOTPICT")) plat_present(); } break;

    /* ---- CopyBits: blit between any src/dst (screen <-> guest-memory bitmap) ---- */
    case 0xA8EC: /*CopyBits*/ {
        (void)pop32();                       /* maskRgn */
        int16_t mode=pop16();
        uint32_t drp=pop32(), srp=pop32(), dstB=pop32(), srcB=pop32();
        uint32_t sbase=m68k_r32(srcB); int srb=m68k_r16(srcB+4)&0x3FFF;
        int sbt=(int16_t)m68k_r16(srcB+6), sbl=(int16_t)m68k_r16(srcB+8);
        uint32_t dbase=m68k_r32(dstB); int drb=m68k_r16(dstB+4)&0x3FFF;
        int dbt=(int16_t)m68k_r16(dstB+6), dbl=(int16_t)m68k_r16(dstB+8);
        Rect s=rd_rect(srp), d=rd_rect(drp);
        if(getenv("MRGFX")) fprintf(stderr,"[gfx] CopyBits src=%x dst=%x sR=%d,%d,%d,%d dR=%d,%d,%d,%d\n",
            sbase,dbase,s.top,s.left,s.bottom,s.right,d.top,d.left,d.bottom,d.right);
        int sw=s.right-s.left, sh=s.bottom-s.top, dw=d.right-d.left, dh=d.bottom-d.top;
        int src_screen=(sbase==screen_base()||sbase==0), dst_screen=(dbase==screen_base()||dbase==0);
        int inv=((mode&0x24)==0x24);         /* notSrcCopy: invert */
        if(sw>0&&sh>0&&dw>0&&dh>0){
            for(int y=0;y<dh;y++){ int sy=(s.top-sbt)+y*sh/dh;
                for(int x=0;x<dw;x++){ int sx=(s.left-sbl)+x*sw/dw; int bit=0;
                    if(src_screen){ int gx=sx+sbl,gy=sy+sbt; bit=(gx>=0&&gx<QD_W&&gy>=0&&gy<QD_H)?qd_fb[gy][gx]:0; }
                    else if(srb>0){ uint32_t a=sbase+(uint32_t)sy*srb+(sx>>3); bit=a<M.memsize?(m68k_r8(a)>>(7-(sx&7)))&1:0; }
                    if(inv) bit=!bit;
                    int dpx=d.left+x, dpy=d.top+y;
                    if(dst_screen){ if(dpx>=0&&dpx<QD_W&&dpy>=0&&dpy<QD_H) qd_fb[dpy][dpx]=(uint8_t)bit; }
                    else if(drb>0){ int dlx=dpx-dbl,dly=dpy-dbt; if(dlx>=0&&dly>=0){ uint32_t a=dbase+(uint32_t)dly*drb+(dlx>>3);
                        if(a<M.memsize){ uint8_t bb=m68k_r8(a),mk=0x80u>>(dlx&7); m68k_w8(a,bit?(bb|mk):(bb&(uint8_t)~mk)); } } }
                } }
        } } break;

    /* ---- Dialogs / misc ---- */
    /* Alerts are a dialog built from an 'ALRT': boundsRect(8), itemsID(2),
     * stages(2). Draw it and run it modally like any other dialog. */
    case 0xA985: /*Alert*/ case 0xA986: /*StopAlert*/ case 0xA987: /*NoteAlert*/
    case 0xA988: /*CautionAlert*/ {
        (void)pop32();                                  /* filterProc */
        int16_t id = pop16();
        uint32_t alrth = res_get(0x414C5254u, id);      /* 'ALRT' */
        uint32_t alrt  = alrth ? m68k_r32(alrth) : 0;
        Rect b; int ditl_id = 0;
        if(alrt){ b = rd_rect(alrt); ditl_id = (int16_t)m68k_r16(alrt+8); }
        else rect_set(&b, 100, 90, 412, 230);
        if(b.right<=b.left || b.bottom<=b.top) rect_set(&b, 100, 90, 412, 230);
        uint32_t ditlh = res_get(0x4449544Cu, ditl_id); /* 'DITL' */
        uint32_t d = make_dialog(0, ditlh ? m68k_r32(ditlh) : 0, &b);
        int hit = d ? dlg_modal(d) : 1;
        if(d) dlg_dispose(d);
        g_front_dlg = 0;
        ret16((uint16_t)hit);
    } break;
    case 0xA895: /*ShutDown*/ M.d[0]=0; break;   /* selector-dispatched; nothing to do */
    case 0xA9F5: /*GetAppParms(VAR apName; VAR apRefNum; VAR apParam)*/ {
        uint32_t ap=pop32(), rn=pop32(), nm=pop32();
        if(nm){ const char *n=app_name(); int l=(int)strlen(n); if(l>31)l=31;
                m68k_w8(nm,(uint8_t)l);
                for(int i=0;i<l;i++) m68k_w8(nm+1+i,(uint8_t)n[i]); }
        if(rn) m68k_w16(rn,1);                  /* the app's own resource file */
        if(ap) m68k_w32(ap, 0);                 /* no Finder startup document */
    } break;
    /* ---- File Manager (files.c) ----
     * Register-based: A0 is the parameter block, D0 the result. Handled in
     * their own module because a title's documents are served from its media,
     * not from the resource fork this file already owns. */
    case 0xA000: case 0xA200: /*Open / HOpen*/
    case 0xA00A: case 0xA20A: /*OpenRF / HOpenRF*/
    case 0xA002: /*Read*/     case 0xA001: /*Close*/
    case 0xA003: /*Write*/    case 0xA011: /*GetEOF*/   case 0xA012: /*SetEOF*/
    case 0xA018: /*GetFPos*/  case 0xA044: /*SetFPos*/
    case 0xA00C: case 0xA20C: /*GetFileInfo / HGetFileInfo*/
    case 0xA00D: /*SetFileInfo*/ case 0xA008: /*Create*/ case 0xA009: /*Delete*/
    case 0xA010: /*Allocate*/ case 0xA013: /*FlushVol*/ case 0xA014: /*GetVol*/
    case 0xA015: /*SetVol*/   case 0xA017: /*Eject*/    case 0xA035: /*OffLine*/
        if(!fs_trap(w)) M.d[0]=(uint32_t)(-43);
        break;
    case 0xA060: case 0xA260: /*FSDispatch / HFSDispatch*/
        fs_dispatch(w); break;

    /* ---- Standard File (Pack3) ----
     * When a title cannot find a document it asks the user, and with no answer
     * it asks forever -- HyperCard spins here once its search for the Home
     * stack comes up short. There is no file dialog to put up in a headless
     * recomp, so the answer comes from MRDOC, which names the document to open;
     * without it the reply is "cancelled", which is also a real answer.
     *
     * SFReply: good(1), copy(1), fType(4), vRefNum(2), version(2), fName(Str255).
     * Auto-pop: the glue pops its own return address, pushes the selector
     * under it and traps, so the address comes off first and goes back on at
     * the end -- for the m68k_rts the lifter emits after an auto-pop trap. */
    case 0xA9EA: { /*Pack3 -- Standard File*/
        uint32_t ra = pop32();
        int16_t sel = (int16_t)pop16();
        uint32_t reply = 0;
        switch(sel){
        case 2: /*SFGetFile(where,prompt,filter,numTypes,typeList,hook,reply)*/
            reply=pop32(); (void)pop32(); (void)pop32(); (void)pop16();
            (void)pop32(); (void)pop32(); (void)pop32(); break;
        case 4: /*SFPGetFile: +dlgID, filterProc*/
            reply=pop32(); (void)pop32(); (void)pop32(); (void)pop32();
            (void)pop32(); (void)pop16(); (void)pop32(); (void)pop32();
            (void)pop32(); break;
        case 1: /*SFPutFile*/
            reply=pop32(); (void)pop32(); (void)pop32(); (void)pop32();
            (void)pop32(); break;
        case 3: /*SFPPutFile*/
            reply=pop32(); (void)pop32(); (void)pop32(); (void)pop32();
            (void)pop32(); (void)pop32(); (void)pop32(); break;
        default: break;
        }
        const char *doc = getenv("MRDOC");
        int give = (sel==2 || sel==4) && doc && *doc;
        if(reply){
            m68k_w8 (reply+0, (uint8_t)(give?1:0));     /* good */
            m68k_w8 (reply+1, 0);                        /* copy */
            for(int i=0;i<4;i++) m68k_w8(reply+2+i, (uint8_t)"STAK"[i]);
            m68k_w16(reply+6, (uint16_t)(-1));           /* vRefNum */
            m68k_w16(reply+8, 0);                        /* version */
            int l = give ? (int)strlen(doc) : 0; if(l>63) l=63;
            m68k_w8(reply+10, (uint8_t)l);
            for(int i=0;i<l;i++) m68k_w8(reply+11+i, (uint8_t)doc[i]);
        }
        if(getenv("MRFILE"))
            fprintf(stderr,"[File] StandardFile selector %d -> %s\n",
                    sel, give ? doc : "cancelled");
        SP -= 4; m68k_w32(SP, ra);          /* put the return address back */
    } break;

    /* ---- Resource Manager (serve the app's own resources) ---- */
    case 0xA9A0: /*GetResource*/ case 0xA81F: /*Get1Resource*/ {
        int16_t id=pop16(); uint32_t ty=pop32();
        /* MRPARSE: HyperTalk's syntax errors are STR# 1002. Probing here rather
         * than at the ParamText that finally shows the text puts us ~8 frames
         * closer to the parser, where its cursor is still on the stack. */
        if(getenv("MRPARSE") && ty==0x53545223u && id==1002) mr_text_probe("STR#1002");
        m68k_w32(SP, res_get(ty,id)); } break;
    case 0xA9A1: /*GetNamedResource*/ { (void)pop32(); (void)pop32(); m68k_w32(SP,0); } break;
    case 0xA9BC: /*GetPicture*/ { int16_t id=pop16(); m68k_w32(SP, res_get(0x50494354u,id)); } break; /*'PICT'*/
    case 0xA9BF: /*GetRMenu/GetMenu*/ { int16_t id=pop16(); m68k_w32(SP, res_get(0x4D454E55u,id)); } break; /*'MENU'*/
    case 0xA9BA: /*GetString*/ { int16_t id=pop16(); m68k_w32(SP, res_get(0x53545220u,id)); } break; /*'STR '*/
    case 0xA9A5: /*SizeRsrc*/ { uint32_t h=pop32(); uint32_t sz=0;
        for(int i=0;i<g_nres;i++) if(g_res[i].handle==h){ sz=g_res[i].len; break; } ret32(sz); } break;

    /* Resource enumeration. HyperCard walks its own resources by index at
     * startup and stops if it cannot; these are cheap because the table the
     * Resource Manager already serves is the answer. Index is 1-based. */
    case 0xA99D: /*GetIndResource*/ case 0xA80E: { /*Get1IxResource*/
        int16_t ix=pop16(); uint32_t ty=pop32();
        char want[5]; type4(ty,want); int n=0; uint32_t h=0;
        for(int i=0;i<g_nres;i++) if(strcmp(g_res[i].type,want)==0)
            if(++n==ix){ h=res_get(ty, g_res[i].id); break; }
        m68k_w32(SP,h); } break;
    case 0xA99E: /*CountTypes*/ case 0xA81C: { /*Count1Types*/
        int n=0;
        for(int i=0;i<g_nres;i++){ int seen=0;
            for(int j=0;j<i;j++) if(strcmp(g_res[i].type,g_res[j].type)==0){ seen=1; break; }
            if(!seen) n++; }
        ret16((uint16_t)n); } break;
    case 0xA99F: /*GetIndType*/ case 0xA80F: { /*Get1IxType*/
        int16_t ix=pop16(); uint32_t tp=pop32(); int n=0;
        for(int i=0;i<g_nres;i++){ int seen=0;
            for(int j=0;j<i;j++) if(strcmp(g_res[i].type,g_res[j].type)==0){ seen=1; break; }
            if(seen) continue;
            if(++n==ix && tp){ const char *t=g_res[i].type;
                for(int k=0;k<4;k++) m68k_w8(tp+k, t[k]?(uint8_t)t[k]:' '); break; } }
        } break;
    /* GetResInfo(theResource; VAR theID; VAR theType; VAR name). The name is
     * whatever res_add was told, which is nothing today -- so it reports an
     * empty Str255 rather than leaving the caller's buffer untouched, which
     * would read as a stale name. */
    case 0xA9A8: { /*GetResInfo*/
        uint32_t nm=pop32(), tp=pop32(), idp=pop32(), h=pop32();
        for(int i=0;i<g_nres;i++) if(g_res[i].handle==h && h){
            if(idp) m68k_w16(idp,(uint16_t)g_res[i].id);
            if(tp){ const char *t=g_res[i].type;
                for(int k=0;k<4;k++) m68k_w8(tp+k, t[k]?(uint8_t)t[k]:' '); }
            break; }
        if(nm) m68k_w8(nm,0); } break;
    case 0xA9A9: /*SetResInfo*/ (void)pop32(); (void)pop32(); (void)pop16(); break;
    case 0xA9A2: /*LoadResource*/ (void)pop32(); break;  /* already in memory */
    case 0xA99C: /*CountResources*/ case 0xA80D: /*Count1Resources*/ {
        uint32_t ty=pop32(); char want[5]; type4(ty,want); int n=0;
        for(int i=0;i<g_nres;i++) if(strcmp(g_res[i].type,want)==0) n++;
        ret16((uint16_t)n); } break;
    case 0xA9C8: /*SysBeep*/ (void)pop16(); break;      /* no audio path yet */
    case 0xA866: /*StuffHex*/ {                          /* StuffHex(ptr, Str255) */
        uint32_t sp2=pop32(), dst=pop32();
        int n = sp2 ? (int)m68k_r8(sp2) : 0;
        for(int i=0;i+1<n;i+=2){
            int hi=m68k_r8(sp2+1+i), lo=m68k_r8(sp2+2+i), v=0;
            for(int k=0;k<2;k++){
                int c = k ? lo : hi;
                c = (c>='0'&&c<='9') ? c-'0'
                  : (c>='A'&&c<='F') ? c-'A'+10
                  : (c>='a'&&c<='f') ? c-'a'+10 : 0;
                v = (v<<4)|c;
            }
            m68k_w8(dst + i/2, v);
        } } break;
    /* ---- Toolbox Utilities: the handle copiers ----
     * Register-based, like the Memory Manager: a title that asks for one of
     * these and gets nothing reads garbage out of A0 and concludes it is out of
     * memory, which is exactly the dialog HyperCard was putting up. */
    case 0xA9E3: /*PtrToHand -- A0 = src, D0 = size; returns A0 = new handle*/ {
        uint32_t src=M.a[0], n=M.d[0];
        uint32_t np=heap_alloc(n?n:4), nh=heap_alloc(4);
        if(!nh||!np){ M.a[0]=0; M.d[0]=(uint32_t)-108; break; }
        for(uint32_t i=0;i<n;i++) m68k_w8(np+i, m68k_r8(src+i));
        m68k_w32(nh,np); hsz_set(nh,n);
        M.a[0]=nh; M.d[0]=0; } break;
    case 0xA9E2: /*PtrToXHand -- A0 = src, A1 = existing handle, D0 = size*/ {
        uint32_t src=M.a[0], h=M.a[1], n=M.d[0];
        if(!h){ M.d[0]=(uint32_t)-109; break; }
        uint32_t np=heap_alloc(n?n:4);
        if(!np){ M.d[0]=(uint32_t)-108; break; }
        for(uint32_t i=0;i<n;i++) m68k_w8(np+i, m68k_r8(src+i));
        m68k_w32(h,np); hsz_set(h,n);
        M.a[0]=h; M.d[0]=0; } break;
    case 0xA9EF: /*PtrAndHand -- append D0 bytes at A0 to the handle in A1*/ {
        uint32_t src=M.a[0], h=M.a[1], n=M.d[0];
        if(!h){ M.d[0]=(uint32_t)-109; break; }
        uint32_t have=hsz_get(h), op=m68k_r32(h);
        uint32_t np=heap_alloc(have+n?have+n:4);
        if(!np){ M.d[0]=(uint32_t)-108; break; }
        for(uint32_t i=0;i<have;i++) m68k_w8(np+i, m68k_r8(op+i));
        for(uint32_t i=0;i<n;i++)    m68k_w8(np+have+i, m68k_r8(src+i));
        m68k_w32(h,np); hsz_set(h,have+n);
        M.a[0]=h; M.d[0]=0; } break;
    case 0xA9E4: /*HandAndHand -- append the handle in A0 to the one in A1*/ {
        uint32_t sh=M.a[0], h=M.a[1];
        if(!sh||!h){ M.d[0]=(uint32_t)-109; break; }
        uint32_t n=hsz_get(sh), src=m68k_r32(sh);
        uint32_t have=hsz_get(h), op=m68k_r32(h);
        uint32_t np=heap_alloc(have+n?have+n:4);
        if(!np){ M.d[0]=(uint32_t)-108; break; }
        for(uint32_t i=0;i<have;i++) m68k_w8(np+i, m68k_r8(op+i));
        for(uint32_t i=0;i<n;i++)    m68k_w8(np+have+i, m68k_r8(src+i));
        m68k_w32(h,np); hsz_set(h,have+n);
        M.a[0]=h; M.d[0]=0; } break;

    case 0xA9E1: /*HandToHand*/ {                        /* A0 = handle, in and out */
        uint32_t h=M.a[0];
        if(!h){ M.d[0]=(uint32_t)-109; break; }
        uint32_t sz=hsz_get(h), src=m68k_r32(h);
        uint32_t np=heap_alloc(sz?sz:4), nh=heap_alloc(4);
        if(!nh||!np){ M.d[0]=(uint32_t)-108; break; }
        for(uint32_t i=0;i<sz;i++) m68k_w8(np+i, m68k_r8(src+i));
        m68k_w32(nh,np); hsz_set(nh,sz);
        M.a[0]=nh; M.d[0]=0; } break;
    case 0xA9AF: /*ResError*/ ret16(0); M.d[0]=0; break;
    case 0xA994: /*CurResFile*/ ret16((uint16_t)res_cur_file()); break;
    case 0xA997: /*OpenResFile*/ { uint32_t nm=pop32(); ret16((uint16_t)fs_open_resfork(nm)); } break;
    case 0xA998: /*UseResFile*/ res_use_file((int16_t)pop16()); break;
    /* OpenRFPerm(fileName, vRefNum, permission): INTEGER. A document's own
     * resource fork. Left unimplemented this does not merely return nothing --
     * it leaves three arguments on the Pascal stack. */
    case 0xA9C4: { /*OpenRFPerm*/
        (void)pop16(); (void)pop16(); uint32_t nm=pop32();
        ret16((uint16_t)fs_open_resfork(nm)); } break;
    case 0xA99B: /*CloseResFile*/ (void)pop16(); break;
    case 0xA992: /*DetachResource*/ case 0xA9A3: /*ReleaseResource*/ case 0xA9A4: /*LoadResource*/
    case 0xA99A: /*HomeResFile? */ (void)pop32(); break;
    case 0xA9AB: /*AddResource*/ (void)pop32();(void)pop32();(void)pop16();(void)pop32(); break;

    /* ---- Memory Manager (register-based) ---- */
    case 0xA11E: /*NewPtr (and Clear/Sys variants)*/
        { uint32_t want=M.d[0]; M.a[0]=heap_alloc(want?want:16); M.d[0]=M.a[0]?0:-108;
          if(getenv("MRHEAP")) fprintf(stderr,"  NewPtr %u -> %06x  (slot %06x)\n",
              (unsigned)want, (unsigned)M.a[0], (unsigned)SP); } break;
    case 0xA122: /*NewHandle (and Clear/Sys variants)*/
        { uint32_t sz=M.d[0]; uint32_t p=heap_alloc(sz?sz:16); uint32_t hh=heap_alloc(4);
        if(hh){ m68k_w32(hh,p); hsz_set(hh,sz); } M.a[0]=hh; M.d[0]=hh?0:-108; } break;
    case 0xA166: /*NewEmptyHandle*/ { uint32_t hh=heap_alloc(4);
        if(hh){ m68k_w32(hh,0); hsz_set(hh,0); } M.a[0]=hh; M.d[0]=hh?0:-108; } break;
    case 0xA01F: /*DisposePtr*/ case 0xA023: /*DisposeHandle*/ M.d[0]=0; break;
    case 0xA02E: /*BlockMove*/ { uint32_t src=M.a[0],dst=M.a[1],n=M.d[0];
        /* Bounds-checked like every other guest access. Indexing M.mem directly
         * reads and writes *outside* the guest's address space when a pointer
         * is stale, corrupting the host's own heap -- which then shows up as
         * damage anywhere at all, with nothing to connect it back to here.
         * Overlapping moves must also work: BlockMove is memmove, not memcpy. */
        if(src < dst && dst - src < n)
            for(uint32_t i=n; i-- > 0; ) m68k_w8(dst+i, m68k_r8(src+i));
        else
            for(uint32_t i=0;i<n;i++) m68k_w8(dst+i, m68k_r8(src+i));
        M.d[0]=0; } break;
    case 0xA029: /*HLock*/ case 0xA02A: /*HUnlock*/ case 0xA02B: /*EmptyHandle*/
    case 0xA049: /*HPurge*/ case 0xA04A: /*HNoPurge*/ case 0xA04B: /*SetGrowZone*/
    case 0xA04C: /*CompactMem*/ case 0xA064: /*MoveHHi*/ case 0xA069: /*HGetState*/
    case 0xA06A: /*HSetState*/ break;


    /* ---- QuickDraw regions (bounding-box model; see rgn_* above) ---- */
    case 0xA8D8: /*NewRgn*/ ret32(rgn_alloc()); break;
    case 0xA8D9: /*DisposeRgn*/ (void)pop32(); break;
    case 0xA8DC: /*CopyRgn*/ { uint32_t d=pop32(),s2=pop32(); Rect r=rgn_get(s2); rgn_put(d,&r); } break;
    case 0xA8DD: /*SetEmptyRgn*/ { uint32_t h=pop32(); Rect z={0,0,0,0}; rgn_put(h,&z); } break;
    case 0xA8DE: /*SetRectRgn*/ { int16_t b=pop16(),rt=pop16(),t=pop16(),l=pop16(); uint32_t h=pop32();
        Rect r; rect_set(&r,l,t,rt,b); rgn_put(h,&r); } break;
    case 0xA8DF: /*RectRgn*/ { uint32_t rp=pop32(),h=pop32(); Rect r=rd_rect(rp); rgn_put(h,&r); } break;
    case 0xA8E0: /*OffsetRgn*/ { int16_t dv=pop16(),dh=pop16(); uint32_t h=pop32();
        Rect r=rgn_get(h); rect_offset(&r,dh,dv); rgn_put(h,&r); } break;
    case 0xA8E1: /*InsetRgn*/ { int16_t dv=pop16(),dh=pop16(); uint32_t h=pop32();
        Rect r=rgn_get(h); rect_inset(&r,dh,dv); rgn_put(h,&r); } break;
    case 0xA8E4: /*SectRgn*/ { uint32_t d=pop32(),b=pop32(),a=pop32();
        Rect ra=rgn_get(a),rb=rgn_get(b),o; if(!rect_sect(&ra,&rb,&o)) rect_set(&o,0,0,0,0);
        rgn_put(d,&o); } break;
    case 0xA8E5: /*UnionRgn*/ { uint32_t d=pop32(),b=pop32(),a=pop32();
        Rect ra=rgn_get(a),rb=rgn_get(b),o; int ea=rect_empty(&ra),eb=rect_empty(&rb);
        if(ea&&eb) rect_set(&o,0,0,0,0); else if(ea) o=rb; else if(eb) o=ra;
        else rect_union(&ra,&rb,&o);
        rgn_put(d,&o); } break;
    case 0xA8E6: /*DiffRgn*/ { uint32_t d=pop32(),b=pop32(),a=pop32();
        Rect ra=rgn_get(a),rb=rgn_get(b),o;
        /* only a full cover is exact in the bbox model; otherwise keep a */
        if(rect_covers(&rb,&ra)) rect_set(&o,0,0,0,0); else o=ra;
        rgn_put(d,&o); } break;
    case 0xA8E7: /*XOrRgn*/ { uint32_t d=pop32(),b=pop32(),a=pop32();
        Rect ra=rgn_get(a),rb=rgn_get(b),o; rect_union(&ra,&rb,&o); rgn_put(d,&o); } break;
    case 0xA8E2: /*EmptyRgn*/ { Rect r=rgn_get(pop32()); ret16(rect_empty(&r)?1:0); } break;
    case 0xA8E3: /*EqualRgn*/ { uint32_t b=pop32(),a=pop32(); Rect ra=rgn_get(a),rb=rgn_get(b);
        ret16((ra.top==rb.top&&ra.left==rb.left&&ra.bottom==rb.bottom&&ra.right==rb.right)?1:0); } break;
    case 0xA8E8: /*PtInRgn*/ { uint32_t h=pop32(),pt=pop32(); int ph,pv; pt_unpack(pt,&ph,&pv);
        Rect r=rgn_get(h); ret16(pt_in_rect(ph,pv,&r)?1:0); } break;
    case 0xA8E9: /*RectInRgn*/ { uint32_t h=pop32(),rp=pop32();
        Rect a=rd_rect(rp),b=rgn_get(h),o; ret16(rect_sect(&a,&b,&o)?1:0); } break;
    case 0xA8D2: /*FrameRgn*/ { Rect r=rgn_get(pop32()); qd_frame_rect(&r); } break;
    case 0xA8D3: /*PaintRgn*/ { Rect r=rgn_get(pop32()); qd_paint_rect(&r); } break;
    case 0xA8D4: /*EraseRgn*/ { Rect r=rgn_get(pop32()); qd_erase_rect(&r); } break;
    case 0xA8D5: /*InverRgn*/ { Rect r=rgn_get(pop32()); qd_invert_rect(&r); } break;
    case 0xA8D6: /*FillRgn*/ { (void)pop32(); Rect r=rgn_get(pop32()); qd_fill_rect(&r,1); } break;
    case 0xA879: /*SetClip*/ { Rect r=rgn_get(pop32()); qd_set_clip(&r);
        if(getenv("MRGFX")) fprintf(stderr,"[gfx] SetClip %d,%d,%d,%d\n",r.top,r.left,r.bottom,r.right); } break;
    case 0xA87A: /*GetClip*/ { uint32_t h=pop32(); Rect r; qd_get_clip(&r); rgn_put(h,&r); } break;
    case 0xA8DA: /*OpenRgn*/ break;   /* region recording: the clip stands in */
    case 0xA8DB: /*CloseRgn*/ { uint32_t h=pop32(); Rect r; qd_get_clip(&r); rgn_put(h,&r); } break;

    /* ---- Dialog Manager ---- */
    case 0xA97C: /*GetNewDialog*/ {
        (void)pop32();                              /* behind */
        uint32_t dstor=pop32(); int16_t id=pop16();
        uint32_t dlogh=res_get(0x444C4F47u,id);     /* 'DLOG' */
        uint32_t dlog=dlogh?m68k_r32(dlogh):0;
        Rect bounds; int ditl_id=0;
        /* DLOG: boundsRect(8), procID(2), visible(2), goAwayFlag(2), refCon(4),
         * itemsID(2) at 18, then the title as a Str255. Reading the id at 20
         * lands on the title's length byte and first character instead -- for
         * HyperCard's "Error" dialog that reads as item list 1349, which does
         * not exist, so the dialog comes up empty. */
        if(dlog){ bounds=rd_rect(dlog); ditl_id=(int16_t)m68k_r16(dlog+18); }
        else rect_set(&bounds,100,80,412,262);
        uint32_t ditlh=res_get(0x4449544Cu,ditl_id); /* 'DITL' */
        ret32(make_dialog(dstor, ditlh?m68k_r32(ditlh):0, &bounds));
    } break;
    case 0xA97D: /*NewDialog*/ {
        uint32_t items=pop32(); (void)pop32(); (void)pop16();   /* refCon, goAwayFlag */
        (void)pop32(); (void)pop16(); (void)pop16(); (void)pop32(); /* behind, procID, visible, title */
        uint32_t bp=pop32(), dstor=pop32();
        Rect b; if(bp) b=rd_rect(bp); else rect_set(&b,100,80,412,262);
        ret32(make_dialog(dstor, items?m68k_r32(items):0, &b));
    } break;
    case 0xA983: /*DisposeDialog*/ { uint32_t d=pop32(); dlg_dispose(d);
        if(g_front_dlg==d) g_front_dlg=0; } break;
    case 0xA981: /*DrawDialog*/ case 0xA978: /*UpdtDialog*/ { uint32_t d=pop32();
        if(w==0xA978) (void)pop32();               /* UpdtDialog also takes updateRgn */
        dlg_draw(d); } break;
    case 0xA991: /*ModalDialog*/ { uint32_t ihp=pop32(); (void)pop32();  /* filterProc */
        int hit=dlg_modal(g_front_dlg); if(ihp) m68k_w16(ihp,hit); } break;
    case 0xA98D: /*GetDialogItem*/ {
        uint32_t bp=pop32(), hp=pop32(), tp=pop32(); int16_t n=pop16(); uint32_t d=pop32();
        int ty=0; uint32_t hh=0; Rect box={0,0,0,0};
        dlg_get_item(d,n,&ty,&hh,&box);
        if(tp) m68k_w16(tp,ty); if(hp) m68k_w32(hp,hh); if(bp) wr_rect(bp,&box);
    } break;
    case 0xA98E: /*SetDialogItem*/ {
        uint32_t bp=pop32(); (void)pop32(); (void)pop16(); int16_t n=pop16(); uint32_t d=pop32();
        if(bp){ Rect b=rd_rect(bp); dlg_set_item_rect(d,n,&b); }
    } break;
    case 0xA990: /*GetDialogItemText*/ { uint32_t out=pop32(), h=pop32(); dlg_get_text_h(h,out); } break;
    case 0xA98F: /*SetDialogItemText*/ { uint32_t str=pop32(), h=pop32(); dlg_set_text_h(h,str); } break;
    case 0xA827: /*HideDialogItem*/ { int16_t n=pop16(); uint32_t d=pop32(); dlg_hide_item(d,n,1); } break;
    case 0xA828: /*ShowDialogItem*/ { int16_t n=pop16(); uint32_t d=pop32(); dlg_hide_item(d,n,0); } break;
    case 0xA98B: /*ParamText*/ { uint32_t p3=pop32(),p2=pop32(),p1=pop32(),p0=pop32();
        /* ^0-^3 are what an alert actually says. A title that reports an error
         * by number puts the number here, so this is often the only place the
         * program tells you what went wrong. */
        if(getenv("MRTRACE")){
            uint32_t ps[4]={p0,p1,p2,p3};
            for(int k=0;k<4;k++){ if(!ps[k]) continue; int l=m68k_r8(ps[k]); if(!l) continue;
                fprintf(stderr,"  ParamText ^%d = \"", k);
                for(int i=0;i<l&&i<80;i++) fputc((int)m68k_r8(ps[k]+1+i), stderr);
                fprintf(stderr,"\"\n"); } }
        /* MRPARSE: a HyperTalk syntax error arrives here as text, and the
         * parser's position is still live in the registers. Find the script in
         * guest memory, then report every register pointing into it -- that
         * turns "Can't understand what's after end" into an exact line. */
        if(getenv("MRPARSE")) mr_text_probe("ParamText");
        dlg_param_text(p0,p1,p2,p3); } break;
    case 0xA984: /*FindDialogItem*/ { uint32_t pt=pop32(), d=pop32(); int ph,pv;
        pt_unpack(pt,&ph,&pv); ret16((uint16_t)(int16_t)dlg_find_item(d,ph,pv)); } break;
    case 0xA97F: /*IsDialogEvent*/ { (void)pop32(); ret16(g_front_dlg?1:0); } break;
    case 0xA980: /*DialogSelect*/ { uint32_t ip=pop32(), dp=pop32(); (void)pop32();
        if(dp) m68k_w32(dp,g_front_dlg); if(ip) m68k_w16(ip,0); ret16(0); } break;
    case 0xA979: /*CouldDialog*/ case 0xA97A: /*FreeDialog*/ (void)pop16(); break;

    /* ---- Control Manager ---- */
    case 0xA954: /*NewControl*/ {
        uint32_t refcon=pop32(); (void)pop16(); (void)pop16(); (void)pop16(); (void)pop16();
        (void)pop16(); uint32_t title=pop32(); uint32_t bp=pop32(); uint32_t owner=pop32();
        (void)refcon;
        uint8_t t[256]; int tl=title?(int)m68k_r8(title):0; if(tl>254)tl=254;
        t[0]=(uint8_t)tl; for(int i=0;i<tl;i++) t[i+1]=(uint8_t)m68k_r8(title+1+i);
        Rect b; if(bp) b=rd_rect(bp); else rect_set(&b,0,0,0,0);
        ret32(ctl_new(owner,&b,t,1,0,0,1,heap_alloc(300)));
    } break;
    case 0xA9BE: /*GetNewControl*/ { uint32_t owner=pop32(); (void)pop16();
        Rect b; rect_set(&b,0,0,20,80);
        ret32(ctl_new(owner,&b,NULL,1,0,0,1,heap_alloc(300))); } break;
    case 0xA955: /*DisposeControl*/ (void)pop32(); break;
    case 0xA963: /*SetControlValue*/ { int16_t v=pop16(); ctl_set_value(pop32(),v); } break;
    case 0xA960: /*GetControlValue*/ { ret16((uint16_t)(int16_t)ctl_value(pop32())); } break;
    case 0xA95D: /*HiliteControl*/ { int16_t h=pop16(); uint32_t c=pop32();
        ctl_set_hilite(c,h); ctl_draw(c); } break;
    case 0xA969: /*DrawControls*/ (void)pop32(); if(g_front_dlg) dlg_draw(g_front_dlg); break;
    case 0xA968: /*TrackControl*/ { (void)pop32(); (void)pop32(); uint32_t c=pop32();
        ret16(c?1:0); } break;   /* inPart: the control body */
    case 0xA96C: /*FindControl*/ { uint32_t cp=pop32(); (void)pop32(); uint32_t pt=pop32();
        int ph,pv; pt_unpack(pt,&ph,&pv); (void)ph; (void)pv;
        if(cp) m68k_w32(cp,0); ret16(0); } break;
    case 0xA95F: /*SetControlTitle*/ { uint32_t t=pop32(), c=pop32();
        if(c&&t){ uint8_t b[256]; int n=(int)m68k_r8(t); if(n>254)n=254;
            b[0]=(uint8_t)n; for(int i=0;i<n;i++) b[i+1]=(uint8_t)m68k_r8(t+1+i);
            for(int i=0;i<=b[0];i++) m68k_w8(c+40+i,b[i]); } } break;
    case 0xA95E: /*ShowControl*/ { uint32_t c=pop32(); if(c) m68k_w8(c+16,0xFF); ctl_draw(c); } break;

    /* ---- Toolbox utilities ---- */
    case 0xA85D: /*BitTst*/ { uint32_t bit=pop32(), ptr=pop32();
        /* Mac bit 0 is the most significant bit of the first byte */
        uint32_t byte=m68k_r8(ptr + (bit>>3)); ret16(((byte>>(7-(bit&7)))&1)?1:0); } break;
    case 0xA85E: /*BitSet*/ { uint32_t bit=pop32(), ptr=pop32(); uint32_t a=ptr+(bit>>3);
        m68k_w8(a, m68k_r8(a) | (0x80u>>(bit&7))); } break;
    case 0xA85F: /*BitClr*/ { uint32_t bit=pop32(), ptr=pop32(); uint32_t a=ptr+(bit>>3);
        m68k_w8(a, m68k_r8(a) & (uint8_t)~(0x80u>>(bit&7))); } break;
    case 0xA858: /*BitAnd*/ { uint32_t b=pop32(),a=pop32(); ret32(a&b); } break;
    case 0xA85B: /*BitOr*/  { uint32_t b=pop32(),a=pop32(); ret32(a|b); } break;
    case 0xA85A: /*BitNot*/ { uint32_t a=pop32(); ret32(~a); } break;
    case 0xA85C: /*BitShift*/ { int16_t c=pop16(); uint32_t v=pop32();
        ret32(c>=0 ? (v<<(c&31)) : (v>>((-c)&31))); } break;
    case 0xA868: /*FixMul*/ { int32_t b=(int32_t)pop32(), a=(int32_t)pop32();
        ret32((uint32_t)(int32_t)(((int64_t)a*(int64_t)b)>>16)); } break;
    case 0xA869: /*FixRatio*/ { int16_t den=(int16_t)pop16(), num=(int16_t)pop16();
        ret32(den ? (uint32_t)(int32_t)(((int32_t)num<<16)/den) : 0x7FFFFFFFu); } break;
    case 0xA86C: /*FixRound*/ { int32_t x=(int32_t)pop32();
        ret16((uint16_t)(int16_t)((x + 0x8000) >> 16)); } break;
    case 0xA861: /*Random*/ { g_rand = g_rand*1103515245u + 12345u;
        ret16((uint16_t)(g_rand>>16)); } break;
    case 0xA88C: /*StringWidth*/ { uint32_t sp=pop32();
        ret16((uint16_t)qd_text_width(sp?(int)m68k_r8(sp):0)); } break;
    case 0xA886: /*TextWidth*/ { int16_t cnt=pop16(); (void)pop16(); (void)pop32();
        ret16((uint16_t)qd_text_width(cnt<0?0:cnt)); } break;
    /* One full-screen window at the origin, so global and local coincide. */
    case 0xA870: /*LocalToGlobal*/ case 0xA871: /*GlobalToLocal*/ (void)pop32(); break;
    case 0xA9F1: /*UnLoadSeg*/ (void)pop32(); break;   /* every segment is resident */
    case 0xA8B0: /*FrameRoundRect*/ { (void)pop16(); (void)pop16();
        Rect r=rd_rect(pop32()); qd_frame_rect(&r); } break;   /* square corners */
    case 0xA8B1: /*PaintRoundRect*/ { (void)pop16(); (void)pop16();
        Rect r=rd_rect(pop32()); qd_paint_rect(&r); } break;
    case 0xA8B2: /*EraseRoundRect*/ { (void)pop16(); (void)pop16();
        Rect r=rd_rect(pop32()); qd_erase_rect(&r); } break;
    case 0xA8F2: /*PicComment*/ { (void)pop32(); (void)pop16(); (void)pop16(); } break;

    /* ---- Scrap Manager (no system scrap; clipboard is out of scope) ---- */
    case 0xA9FC: /*ZeroScrap*/ case 0xA9FB: /*UnloadScrap*/ case 0xA9FA: /*LoadScrap*/
        ret32(0); break;
    case 0xA9FD: /*GetScrap*/ { (void)pop32(); (void)pop32(); (void)pop32();
        ret32((uint32_t)-102); } break;    /* noTypeErr */
    case 0xA9FE: /*PutScrap*/ { (void)pop32(); (void)pop32(); (void)pop32();
        ret32(0); } break;

    /* ---- Memory Manager (register-based) ---- */
    case 0xA025: /*GetHandleSize*/ M.d[0]=hsz_get(M.a[0]); break;
    case 0xA024: /*SetHandleSize*/ { uint32_t h=M.a[0], want=M.d[0];
        if(!h){ M.d[0]=(uint32_t)-109; break; }
        int known=hsz_known(h); uint32_t have=hsz_get(h);
        if(getenv("MRHEAP")) fprintf(stderr,
            "  SetHandleSize h=%06x *h=%06x %s%u -> %u\n",
            h, m68k_r32(h), known?"":"(unknown)", have, want);
        if(known && want<=have){ hsz_set(h,want); M.d[0]=0; break; }
        uint32_t np=heap_alloc(want);
        if(!np){ M.d[0]=(uint32_t)-108; break; }
        uint32_t op=m68k_r32(h);
        /* A handle the HAL never recorded has no known length, and copying
         * `have` -- zero -- would hand back a wiped block. Copy what the caller
         * asked for instead: this heap never reuses a block, so reading past
         * the old one is harmless and the surplus is beyond what is asked for.
         * Silently emptying a handle here is indistinguishable, further on,
         * from the data having been garbage all along. */
        uint32_t n = known ? have : want;
        if(n>want) n=want;
        for(uint32_t i=0;i<n;i++) m68k_w8(np+i, m68k_r8(op+i));
        m68k_w32(h,np); hsz_set(h,want); M.d[0]=0; } break;
    case 0xA02D: /*SetApplLimit*/ M.d[0]=0; break;   /* bump heap: no limit to set */
    /* SysEnvirons(D0 = versRequested, A0 = SysEnvRec*). Describe a machine that
     * matches what this runtime actually is: a 68000 with a 1-bit screen and a
     * 128K ROM. Claiming a 68020 or Color QuickDraw would send titles down code
     * paths the HAL does not implement. */
    case 0xA090: /*SysEnvirons*/ {
        uint32_t r = M.a[0];
        if(r){
            m68k_w16(r+0,  1);       /* environsVersion */
            m68k_w16(r+2,  5);       /* machineType: envMacSE -- has the 128K ROM */
            m68k_w16(r+4,  0x0605);  /* systemVersion 6.0.5 */
            m68k_w16(r+6,  1);       /* processor: env68000, what m68k.c models */
            m68k_w8 (r+8,  0);       /* hasFPU */
            m68k_w8 (r+9,  0);       /* hasColorQD */
            m68k_w16(r+10, 1);       /* keyBoardType */
            m68k_w16(r+12, 0);       /* atDrvrVersNum */
            m68k_w16(r+14, 0);       /* sysVRefNum */
        }
        M.d[0]=0;
    } break;
    case 0xA834: /*SetFScaleDisable*/ case 0xA814: /*SetFractEnable*/
        (void)pop16(); break;              /* font scaling: one fixed-pitch face */
    /* StackSpace: how much room is left before the stack would run into
     * anything else. A recursion guard reads this and stops when it looks
     * small, so an unimplemented one reads as a stack already full -- which is
     * the "too much recursion" a title then reports. The floor is where this
     * runtime is willing to let the stack grow to; everything below is
     * low-memory globals. */
    case 0xA065: /*StackSpace*/ {
        const uint32_t floor = 0x00010000u;
        M.d[0] = SP > floor ? SP - floor : 0; } break;
    case 0xA01C: /*FreeMem*/ M.d[0]=heap_free(); break;
    case 0xA11A: /*GetZone*/ M.a[0]=0; M.d[0]=0; break;
    /* Report what is actually left rather than a flattering constant: a title
     * that is told there is room and then cannot allocate has no way to cope,
     * where one told the truth can trim its appetite. */
    case 0xA11D: /*MaxMem*/ case 0xA162: /*PurgeSpace*/ case 0xA061: /*MaxBlock*/
        M.d[0]=heap_free(); M.a[0]=heap_free(); break;
    case 0xA126: /*HandleZone*/ M.a[0]=0; M.d[0]=0; break;
    case 0xA128: /*RecoverHandle*/ M.d[0]=0; break;
    /* Feature probing. Code asks "does this trap exist?" by comparing its
     * address with _Unimplemented's ($A89F); equal means absent. Returning 0
     * for everything makes every trap look absent, which is how HyperCard
     * decides it is running on a pre-128K ROM. Hand back a distinct synthetic
     * address per trap instead, and one shared address for _Unimplemented.
     * ponytail: claims every trap exists. A trap we do not actually dispatch
     * still logs and returns -- better than failing the ROM check outright.
     * Narrow this to the dispatched set if a title probes for something it
     * must not use. */
    case 0xA146: /*GetTrapAddress*/ case 0xA346: /*GetOSTrapAddress*/ {
        uint16_t t = (uint16_t)(M.d[0] & 0x0FFF);
        M.a[0] = (t == 0x89F || t == 0x09F) ? 0x40000000u : (0x40000000u | t);
        M.d[0] = 0;
    } break;
    case 0xA047: /*SetTrapAddress*/ M.d[0]=0; break;

    default: logtrap(w); break;
    }
}

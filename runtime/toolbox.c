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
static void push16(uint16_t v){ SP-=2; m68k_w16(SP,v); }
static void push32(uint32_t v){ SP-=4; m68k_w32(SP,v); }

static Rect rd_rect(uint32_t p){ Rect r; r.top=(int16_t)m68k_r16(p); r.left=(int16_t)m68k_r16(p+2);
    r.bottom=(int16_t)m68k_r16(p+4); r.right=(int16_t)m68k_r16(p+6); return r; }
static void wr_rect(uint32_t p,const Rect*r){ m68k_w16(p,r->top); m68k_w16(p+2,r->left);
    m68k_w16(p+4,r->bottom); m68k_w16(p+6,r->right); }

/* ---- bump heap inside M.mem (NewPtr/NewHandle) ---- */
static uint32_t heap_ptr = 0, heap_end = 0;
static void heap_init(void){ heap_ptr = 0x00800000; heap_end = 0x01E00000; }  /* 8..30 MB */
static uint32_t heap_alloc(uint32_t sz){ sz=(sz+3)&~3u; if(heap_ptr+sz>=heap_end) return 0;
    uint32_t p=heap_ptr; heap_ptr+=sz; for(uint32_t i=0;i<sz;i++) M.mem[p+i]=0; return p; }

/* ---- Resource Manager: serve the app's own extracted resources ---- */
typedef struct { char type[6]; int id; const uint8_t *data; int len; uint32_t handle; } Res;
static Res g_res[6000]; static int g_nres;
void res_add(const char *type, int id, const uint8_t *data, int len){
    if(g_nres>=(int)(sizeof g_res/sizeof*g_res)) return;
    Res *r=&g_res[g_nres++]; int i=0;
    for(;i<5&&type[i];i++) r->type[i]=type[i];
    while(i>0&&r->type[i-1]==' ') i--;           /* strip trailing spaces */
    r->type[i]=0; r->id=id; r->data=data; r->len=len; r->handle=0;
}
static void type4(uint32_t t, char *out){ /* 'PICT' long -> stripped string */
    out[0]=t>>24; out[1]=t>>16; out[2]=t>>8; out[3]=t; out[4]=0;
    int n=4; while(n>0&&out[n-1]==' ') out[--n]=0;
}
static uint32_t res_get(uint32_t typelong, int id){
    char want[5]; type4(typelong,want);
    for(int i=0;i<g_nres;i++){
        if(g_res[i].id==id && strcmp(g_res[i].type,want)==0){
            Res *r=&g_res[i];
            if(!r->handle){                       /* lazy: copy into M.mem, make a handle */
                uint32_t p=heap_alloc(r->len); for(int k=0;k<r->len;k++) M.mem[p+k]=r->data[k];
                uint32_t h=heap_alloc(4); m68k_w32(h,p); r->handle=h;
            }
            return r->handle;
        }
    }
    return 0;
}

/* ---- trap normalization: strip auto-pop / flag bits ---- */
static uint16_t norm(uint16_t w){ return (w&0x0800)?(0xA800|(w&0x03FF)):(0xA000|(w&0x00FF)); }

static int g_inited = 0;
void toolbox_init(void){ qd_init(); heap_init(); g_inited=1; }

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
                if(op==0x98){ int ln = rowbytes>250 ? (int)m68k_r16(o) : m68k_r8(o); o += rowbytes>250?2:1;
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

void m68k_trap(uint16_t raw){
    uint16_t w = norm(raw);
    if(getenv("MRTRACE")){ static long n=0; fprintf(stderr,"[%5ld] $%04X\n", n++, w); }
    if(getenv("MRPRESENT")){ static long p=0; if(++p%300==0) plat_present(); }
    switch(w){
    /* ---- init (mostly no-ops for us) ---- */
    case 0xA86E: /*InitGraf*/ (void)pop32(); break;      /* arg: globalsPtr */
    case 0xA8FE: /*InitFonts*/ case 0xA912: /*InitWindows*/ case 0xA930: /*InitMenus*/
    case 0xA9CC: /*TEInit*/    case 0xA063: /*MaxApplZone*/ case 0xA850: /*InitCursor*/
    case 0xA036: /*MoreMasters*/ break;
    case 0xA97B: /*InitDialogs*/ (void)pop32(); break;   /* arg: resumeProc */
    case 0xA032: /*FlushEvents*/ (void)pop32(); break;
    case 0xA9F4: /*ExitToShell*/ fprintf(stderr,"[ExitToShell]\n"); plat_present(); exit(0);

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
    case 0xA8B8: /*EraseOval*/ { Rect r=rd_rect(pop32()); qd_fill_oval(&r,0); } break;
    case 0xA8B9: /*InvertOval*/ { Rect r=rd_rect(pop32()); qd_fill_oval(&r,1); } break;
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
        plat_present();                       /* reaching the event loop == booted */
        if(plat_quit_requested()) exit(0);
        (void)pop16(); uint32_t evp=pop32(); int what=0,msg=0,h=0,v=0;
        plat_pump(); int got=plat_next_event(&what,&msg,&h,&v);
        if(evp){ m68k_w16(evp,what); m68k_w32(evp+2,msg); m68k_w32(evp+6,plat_ticks());
                 int mh,mv; plat_get_mouse(&mh,&mv); m68k_w16(evp+10,mv); m68k_w16(evp+12,mh);
                 m68k_w16(evp+14,0); }
        push16(got?1:0); } break;

    /* ---- cursor / port (mostly no-ops; QuickDraw draws to one framebuffer) ---- */
    case 0xA852: /*HideCursor*/ case 0xA853: /*ShowCursor*/ case 0xA856: /*ObscureCursor*/
    case 0xA9B4: /*SystemTask*/ break;
    case 0xA86F: /*OpenPort*/ { uint32_t p=pop32(); if(p){ Rect s; rect_set(&s,0,0,QD_W,QD_H); wr_rect(p+16,&s);} } break;
    case 0xA875: /*SetPortBits*/ (void)pop32(); break;
    case 0xA9B8: /*GetPattern*/ { (void)pop16(); uint32_t h=heap_alloc(4),p=heap_alloc(8);
        for(int i=0;i<8;i++) M.mem[p+i]=0xFF; if(h)m68k_w32(h,p); push32(h); } break;

    /* ---- Window Manager (thin: return a real WindowRecord so the game can draw) ---- */
    case 0xA913: /*NewWindow*/ {
        uint32_t refcon=pop32(); (void)pop16(); uint32_t behind=pop32(); (void)behind;
        (void)pop16(); (void)pop16(); uint32_t title=pop32(); (void)title;
        uint32_t bounds=pop32(); uint32_t wstor=pop32();
        uint32_t w = wstor ? wstor : heap_alloc(256);
        Rect br = bounds?rd_rect(bounds):(Rect){0,0,QD_H,QD_W};
        Rect pr; rect_set(&pr,0,0,br.bottom-br.top,br.right-br.left);
        wr_rect(w+16, &pr);                 /* GrafPort.portRect */
        m68k_w32(w+0xFC, refcon);           /* WindowRecord.refCon (approx offset) */
        m68k_w32(SP, w);                    /* Pascal result slot */
    } break;
    case 0xA9BD: /*GetNewWindow*/ {
        uint32_t behind=pop32(); (void)behind; uint32_t wstor=pop32(); (void)pop16();
        uint32_t w = wstor ? wstor : heap_alloc(256);
        Rect pr; rect_set(&pr,0,0,QD_H,QD_W); wr_rect(w+16,&pr);
        m68k_w32(SP, w);
    } break;
    case 0xA914: /*GetWMgrPort*/ { uint32_t pp=pop32(); if(pp)m68k_w32(pp,0); } break;
    case 0xA91F: /*SelectWindow*/ case 0xA915: /*ShowWindow*/ case 0xA916: /*HideWindow*/
    case 0xA922: /*BeginUpdate*/ case 0xA923: /*EndUpdate*/ case 0xA928: /*InvalRect*/
    case 0xA92A: /*ValidRect*/ case 0xA904: /*DrawGrowIcon*/ (void)pop32(); break;
    case 0xA924: /*FrontWindow*/ push32(0); break;

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
    case 0xA93B: /*GetMenuBar*/ push32(heap_alloc(4)); break;
    case 0xA948: /*CalcMenuSize*/ (void)pop32(); break;
    case 0xA950: /*CountMItems*/ { (void)pop32(); push16(0); } break;
    case 0xADC0: /*GetNewMBar*/ { (void)pop16(); push32(heap_alloc(4)); } break;
    case 0xA9C9: /*SysError*/ (void)pop16(); break;

    /* ---- DrawPicture: blit real PICT art ---- */
    case 0xA8F6: /*DrawPicture*/ { uint32_t rp=pop32(); uint32_t pich=pop32();
        uint32_t pic = pich ? m68k_r32(pich) : 0; Rect dst = rd_rect(rp);
        if(pic) draw_pict(pic, dst); } break;

    /* ---- CopyBits: blit a source BitMap (in M.mem) to the framebuffer ---- */
    case 0xA8EC: /*CopyBits*/ {
        (void)pop32();                       /* maskRgn */ (void)pop16(); /* mode */
        uint32_t drp=pop32(), srp=pop32(); (void)pop32(); /* dstBits */ uint32_t src=pop32();
        uint32_t base=m68k_r32(src); int rb=m68k_r16(src+4)&0x3FFF;
        int bt=(int16_t)m68k_r16(src+6), bl=(int16_t)m68k_r16(src+8);
        Rect s=rd_rect(srp), d=rd_rect(drp);
        int sw=s.right-s.left, sh=s.bottom-s.top, dw=d.right-d.left, dh=d.bottom-d.top;
        if(sw>0&&sh>0&&dw>0&&dh>0&&rb>0&&base&&base<M.memsize){
            for(int y=0;y<dh;y++){ int sy=s.top-bt + y*sh/dh;
                for(int x=0;x<dw;x++){ int sx=s.left-bl + x*sw/dw;
                    uint32_t a=base+(uint32_t)sy*rb+(sx>>3);
                    int bit = a<M.memsize ? (m68k_r8(a)>>(7-(sx&7)))&1 : 0;
                    int px=d.left+x, py=d.top+y;
                    if(px>=0&&px<QD_W&&py>=0&&py<QD_H) qd_fb[py][px]=bit; } }
        } } break;

    /* ---- Dialogs / misc ---- */
    case 0xA985: /*Alert*/ case 0xA986: /*StopAlert*/ case 0xA987: /*NoteAlert*/
    case 0xA988: /*CautionAlert*/ { (void)pop32(); (void)pop16(); m68k_w16(SP,1); } break;
    case 0xA9F5: /*GetAppParms*/ { uint32_t ap=pop32(),rn=pop32(),nm=pop32();
        if(nm)m68k_w8(nm,0); if(rn)m68k_w16(rn,0); if(ap)m68k_w32(ap,0); } break;
    case 0xA000: /*Open*/ case 0xA002: /*Read*/ case 0xA003: /*Write*/ case 0xA001: /*Close*/
    case 0xA044: /*SetFPos*/ case 0xA018: /*GetFPos*/ case 0xA013: /*FlushVol*/
    case 0xA008: /*Create*/ case 0xA009: /*Delete*/ case 0xA00C: /*GetFileInfo*/
        M.d[0]=0; break;                     /* register-based File Mgr: pretend OK */

    /* ---- Resource Manager (serve the app's own resources) ---- */
    case 0xA9A0: /*GetResource*/ case 0xA81F: /*Get1Resource*/ {
        int16_t id=pop16(); uint32_t ty=pop32(); m68k_w32(SP, res_get(ty,id)); } break;
    case 0xA9A1: /*GetNamedResource*/ { (void)pop32(); (void)pop32(); m68k_w32(SP,0); } break;
    case 0xA9BC: /*GetPicture*/ { int16_t id=pop16(); m68k_w32(SP, res_get(0x50494354u,id)); } break; /*'PICT'*/
    case 0xA9BF: /*GetRMenu/GetMenu*/ { int16_t id=pop16(); m68k_w32(SP, res_get(0x4D454E55u,id)); } break; /*'MENU'*/
    case 0xA9BA: /*GetString*/ { int16_t id=pop16(); m68k_w32(SP, res_get(0x53545220u,id)); } break; /*'STR '*/
    case 0xA9A5: /*SizeRsrc*/ { uint32_t h=pop32(); uint32_t sz=0;
        for(int i=0;i<g_nres;i++) if(g_res[i].handle==h){ sz=g_res[i].len; break; } push32(sz); } break;
    case 0xA9AF: /*ResError*/ push16(0); M.d[0]=0; break;
    case 0xA994: /*CurResFile*/ case 0xA997: /*OpenResFile*/ push16(1); break;
    case 0xA998: /*UseResFile*/ (void)pop16(); break;
    case 0xA99B: /*CloseResFile*/ (void)pop16(); break;
    case 0xA992: /*DetachResource*/ case 0xA9A3: /*ReleaseResource*/ case 0xA9A4: /*LoadResource*/
    case 0xA99A: /*HomeResFile? */ (void)pop32(); break;
    case 0xA9AB: /*AddResource*/ (void)pop32();(void)pop32();(void)pop16();(void)pop32(); break;

    /* ---- Memory Manager (register-based) ---- */
    case 0xA01E: /*(alias) alloc*/ case 0xA11E: /*NewPtr*/ case 0xA51E: /*NewPtrSys*/ { M.a[0]=heap_alloc(M.d[0]?M.d[0]:16); M.d[0]=M.a[0]?0:-108; } break;
    case 0xA122: /*NewHandle*/ { uint32_t p=heap_alloc(M.d[0]); uint32_t hh=heap_alloc(4);
        if(hh)m68k_w32(hh,p); M.a[0]=hh; M.d[0]=hh?0:-108; } break;
    case 0xA01F: /*DisposePtr*/ case 0xA023: /*DisposeHandle*/ M.d[0]=0; break;
    case 0xA02E: /*BlockMove*/ { uint32_t src=M.a[0],dst=M.a[1],n=M.d[0];
        for(uint32_t i=0;i<n;i++) M.mem[dst+i]=M.mem[src+i]; } break;
    case 0xA029: /*HLock*/ case 0xA02A: /*HUnlock*/ case 0xA02B: /*EmptyHandle*/
    case 0xA049: /*HPurge*/ case 0xA04A: /*HNoPurge*/ case 0xA04B: /*SetGrowZone*/
    case 0xA04C: /*CompactMem*/ case 0xA064: /*MoveHHi*/ case 0xA069: /*HGetState*/
    case 0xA06A: /*HSetState*/ break;

    default: logtrap(w); break;
    }
}

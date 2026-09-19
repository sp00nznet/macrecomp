/* hal_selftest.c - drive the Toolbox HAL through real trap calls.
 *
 * Pushes Pascal arguments onto the 68k stack exactly as lifted code would, then
 * calls m68k_trap(). Two halves:
 *
 *   1. checks -- regions, dialogs, and the Pascal result convention, asserted
 *      through the trap interface so a stack-discipline slip is caught here
 *      rather than in a lifted title;
 *   2. a mock scene drawn through QuickDraw traps and dumped to a PGM, so the
 *      whole path (Pascal args -> dispatch -> QuickDraw -> framebuffer) runs.
 *
 * No SDL/display needed. Exits non-zero if any check fails. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* headless stubs for the platform layer (real one is platform_sdl.c) */
uint32_t plat_ticks(void){ return 0; }
void plat_get_mouse(int*h,int*v){ if(h)*h=0; if(v)*v=0; }
int  plat_button(void){ return 0; }
void plat_inject_click(int x,int y){ (void)x;(void)y; }
void plat_post_event(int w,int m){ (void)w;(void)m; }
void plat_pump(void){}
void plat_present(void){}
int  plat_quit_requested(void){ return 0; }
int  plat_next_event(int*w,int*m,int*h,int*v){ (void)m;(void)h;(void)v; if(w)*w=0; return 0; }

static void push16(uint16_t v){ SP-=2; m68k_w16(SP,v); }
static void push32(uint32_t v){ SP-=4; m68k_w32(SP,v); }

static int g_fail;
#define CHECK(cond, ...) do{ if(!(cond)){ g_fail++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } }while(0)

/* scratch allocator in guest memory */
static uint32_t SCRATCH = 0x080000;
static uint32_t alloc(int n){ uint32_t p=SCRATCH; SCRATCH=(SCRATCH+n+1)&~1u; return p; }
static uint32_t rect(int l,int t,int r,int b){ uint32_t p=alloc(8);
    m68k_w16(p,t); m68k_w16(p+2,l); m68k_w16(p+4,b); m68k_w16(p+6,r); return p; }
static uint32_t pstr(const char *s){ int n=(int)strlen(s); uint32_t p=alloc(n+1);
    m68k_w8(p,n); for(int i=0;i<n;i++) m68k_w8(p+1+i,(uint8_t)s[i]); return p; }
static uint32_t point(int h,int v){ return ((uint32_t)(uint16_t)v<<16)|(uint16_t)h; }
static int rd16(uint32_t p){ return (int16_t)m68k_r16(p); }

#define TRAP(w) m68k_trap(w)

/* A Pascal call reserves its result slot *below* the arguments. CALL_BEGIN
 * reserves it, CALL_END checks SP landed back on that slot and frees it. */
static uint32_t g_sp0;
static int g_rsz;
static void call_begin(int rsz){ g_sp0=SP; g_rsz=rsz; SP-=rsz; }
static uint32_t call_end32(void){
    CHECK(SP==g_sp0-(uint32_t)g_rsz, "stack unbalanced: SP=%x expected %x", SP, g_sp0-g_rsz);
    uint32_t v = g_rsz==2 ? m68k_r16(SP) : m68k_r32(SP);
    SP = g_sp0; return v;
}
/* A Pascal Boolean occupies the 2-byte result slot but is read as a BYTE at the
 * slot's address: compiled code does `move.b (a7)+,d0`, which on a big-endian
 * machine takes the HIGH byte. Reading the slot as a word and comparing with 1
 * -- which this check used to do -- passes only against a HAL that puts the
 * value in the wrong half, and every Boolean trap then reads as false to a real
 * title. HyperCard's card composite is gated on exactly this. */
static int call_endbool(void){
    CHECK(SP==g_sp0-(uint32_t)g_rsz, "stack unbalanced: SP=%x expected %x", SP, g_sp0-g_rsz);
    int v = (int)m68k_r8(SP);
    SP = g_sp0; return v;
}

static void test_pascal_results(void){
    printf("Pascal result convention\n");
    /* SectRect(a, b, VAR dst): Boolean */
    uint32_t a=rect(0,0,100,100), b=rect(50,50,150,150), dst=rect(0,0,0,0);
    call_begin(2); push32(a); push32(b); push32(dst); TRAP(0xA8AA);
    CHECK(call_endbool(), "SectRect should report an intersection");
    CHECK(rd16(dst+2)==50 && rd16(dst)==50, "SectRect dst = (50,50,...), got (%d,%d)",
          rd16(dst+2), rd16(dst));

    uint32_t far_=rect(900,900,950,950);
    call_begin(2); push32(a); push32(far_); push32(dst); TRAP(0xA8AA);
    CHECK(!call_endbool(), "disjoint rects should not intersect");

    /* PtInRect(pt, r): Boolean -- the Point is by value, not a pointer */
    call_begin(2); push32(point(10,10)); push32(a); TRAP(0xA8AD);
    CHECK(call_endbool(), "(10,10) is inside (0,0,100,100)");
    call_begin(2); push32(point(500,500)); push32(a); TRAP(0xA8AD);
    CHECK(!call_endbool(), "(500,500) is outside (0,0,100,100)");
}

static void test_regions(void){
    printf("regions\n");
    call_begin(4); TRAP(0xA8D8); uint32_t r1=call_end32();      /* NewRgn */
    call_begin(4); TRAP(0xA8D8); uint32_t r2=call_end32();
    call_begin(4); TRAP(0xA8D8); uint32_t r3=call_end32();
    CHECK(r1 && r2 && r3 && r1!=r2, "NewRgn should return distinct handles");

    call_begin(2); push32(r1); TRAP(0xA8E2);                    /* EmptyRgn */
    CHECK(call_endbool(), "a fresh region is empty");

    /* SetRectRgn(rgn, l, t, r, b) */
    push32(r1); push16(0); push16(0); push16(100); push16(100); TRAP(0xA8DE);
    push32(r2); push16(50); push16(50); push16(150); push16(150); TRAP(0xA8DE);

    call_begin(2); push32(r1); TRAP(0xA8E2);
    CHECK(!call_endbool(), "a region set to a real rect is not empty");

    push32(r1); push32(r2); push32(r3); TRAP(0xA8E4);           /* SectRgn */
    call_begin(2); push32(point(60,60)); push32(r3); TRAP(0xA8E8);  /* PtInRgn */
    CHECK(call_endbool(), "(60,60) is in the 50..100 overlap");
    call_begin(2); push32(point(20,20)); push32(r3); TRAP(0xA8E8);
    CHECK(!call_endbool(), "(20,20) is outside the overlap");

    push32(r1); push32(r2); push32(r3); TRAP(0xA8E5);           /* UnionRgn */
    call_begin(2); push32(point(140,140)); push32(r3); TRAP(0xA8E8);
    CHECK(call_endbool(), "(140,140) is inside the union");

    /* DiffRgn where b covers a: the bbox model represents that one exactly */
    push32(r2); push16(0); push16(0); push16(200); push16(200); TRAP(0xA8DE);
    push32(r1); push32(r2); push32(r3); TRAP(0xA8E6);
    call_begin(2); push32(r3); TRAP(0xA8E2);
    CHECK(call_endbool(), "a - b is empty when b covers a");
}

/* Build a DITL: count-1, then per item 4 placeholder + 8 rect + type + len + data. */
static uint32_t build_ditl(void){
    uint32_t p = alloc(256), o = p;
    m68k_w16(o, 2); o += 2;                    /* 3 items */
    struct { int l,t,r,b,type; const char *s; } items[] = {
        { 20, 60,  80, 80,  4, "OK" },         /* button      */
        { 20, 10, 200, 30,  8, "Hi ^0" },      /* static text */
        { 20, 35, 200, 55, 16, "edit" },       /* edit text   */
    };
    for(int i=0;i<3;i++){
        m68k_w32(o,0); o+=4;
        m68k_w16(o,items[i].t); m68k_w16(o+2,items[i].l);
        m68k_w16(o+4,items[i].b); m68k_w16(o+6,items[i].r); o+=8;
        int n=(int)strlen(items[i].s);
        m68k_w8(o++, items[i].type); m68k_w8(o++, n);
        for(int k=0;k<n;k++) m68k_w8(o++, (uint8_t)items[i].s[k]);
        if(n & 1) o++;
    }
    return p;
}

static void test_dialog(void){
    printf("dialogs\n");
    uint32_t ditl = build_ditl();
    uint32_t items = alloc(4); m68k_w32(items, ditl);       /* Handle to the DITL */
    uint32_t bounds = rect(100,80,412,262);

    /* NewDialog(dStorage, bounds, title, visible, procID, behind, goAway, refCon, items) */
    call_begin(4);
    push32(0); push32(bounds); push32(pstr("T")); push16(1);
    push16(1); push32(0); push16(0); push32(0); push32(items);
    TRAP(0xA97D);
    uint32_t d = call_end32();
    CHECK(d != 0, "NewDialog should return a DialogPtr");
    CHECK(dlg_count(d)==3, "DITL should parse to 3 items, got %d", dlg_count(d));

    /* GetDialogItem(d, 1, VAR type, VAR handle, VAR box) */
    uint32_t tp=alloc(2), hp=alloc(4), bp=alloc(8);
    push32(d); push16(1); push32(tp); push32(hp); push32(bp); TRAP(0xA98D);
    CHECK(rd16(tp)==4, "item 1 is a button (type 4), got %d", rd16(tp));
    CHECK(m68k_r32(hp)!=0, "item 1 should have a handle");
    CHECK(rd16(bp+2)==20 && rd16(bp)==60, "item 1 box = (20,60,..), got (%d,%d)",
          rd16(bp+2), rd16(bp));

    /* the button's handle is a ControlRecord: SetControlValue must stick */
    uint32_t btn = m68k_r32(hp);
    push32(btn); push16(1); TRAP(0xA963);                   /* SetControlValue */
    call_begin(2); push32(btn); TRAP(0xA960);               /* GetControlValue */
    CHECK(call_end32()==1, "control value should read back as 1");

    /* ParamText ^0 expands when the static text is read back */
    push32(pstr("World")); push32(0); push32(0); push32(0); TRAP(0xA98B);
    push32(d); push16(2); push32(tp); push32(hp); push32(bp); TRAP(0xA98D);
    uint32_t out = alloc(256);
    push32(m68k_r32(hp)); push32(out); TRAP(0xA990);        /* GetDialogItemText */
    char got[64]; int n=m68k_r8(out); if(n>63) n=63;
    for(int i=0;i<n;i++) got[i]=(char)m68k_r8(out+1+i); got[n]=0;
    CHECK(strcmp(got,"Hi World")==0, "ParamText should expand ^0, got \"%s\"", got);

    /* SetDialogItemText on the edit field, then read it back */
    push32(d); push16(3); push32(tp); push32(hp); push32(bp); TRAP(0xA98D);
    uint32_t eh = m68k_r32(hp);
    push32(eh); push32(pstr("typed")); TRAP(0xA98F);
    push32(eh); push32(out); TRAP(0xA990);
    n=m68k_r8(out); for(int i=0;i<n;i++) got[i]=(char)m68k_r8(out+1+i); got[n]=0;
    CHECK(strcmp(got,"typed")==0, "SetDialogItemText should stick, got \"%s\"", got);

    /* GetDialogItem returns the box in window-local coordinates, but the window
     * sits at (100,80), so hit-testing takes screen coordinates: item 1's local
     * (20,60,80,80) is (120,140,180,160) on screen.
     * FindDialogItem is 0-based, -1 for a miss. */
    call_begin(2); push32(d); push32(point(130,145)); TRAP(0xA984);
    CHECK((int16_t)call_end32()==0, "point in the button should find item index 0");
    call_begin(2); push32(d); push32(point(30,65)); TRAP(0xA984);
    CHECK((int16_t)call_end32()==-1, "the item's *local* box is not where it is on screen");
    call_begin(2); push32(d); push32(point(400,300)); TRAP(0xA984);
    CHECK((int16_t)call_end32()==-1, "point outside every item should find nothing");

    /* hiding an item takes it out of hit-testing */
    push32(d); push16(1); TRAP(0xA827);                     /* HideDialogItem */
    call_begin(2); push32(d); push32(point(130,145)); TRAP(0xA984);
    CHECK((int16_t)call_end32()==-1, "a hidden item should not be found");

    dlg_draw(d);                                            /* must not crash */
    push32(d); TRAP(0xA983);                                /* DisposeDialog */
    CHECK(dlg_count(d)==0, "disposed dialog should be gone");
}

static void test_memory(void){
    printf("memory manager\n");
    /* NewHandle is $A122; the flag bits above the 9-bit trap number must not
     * fold it onto a different trap. */
    M.d[0]=64; m68k_trap(0xA122);
    uint32_t h=M.a[0];
    CHECK(h!=0 && M.d[0]==0, "NewHandle should return a handle");
    M.a[0]=h; m68k_trap(0xA025);                            /* GetHandleSize */
    CHECK(M.d[0]==64, "GetHandleSize should be 64, got %u", M.d[0]);

    m68k_w8(m68k_r32(h), 0xAB);                             /* mark the block */
    M.a[0]=h; M.d[0]=4096; m68k_trap(0xA024);               /* SetHandleSize: grow */
    CHECK(M.d[0]==0, "SetHandleSize should succeed");
    M.a[0]=h; m68k_trap(0xA025);
    CHECK(M.d[0]==4096, "handle should now be 4096, got %u", M.d[0]);
    CHECK(m68k_r8(m68k_r32(h))==0xAB, "growing a handle must preserve its contents");

    /* NewHandleClear ($A322) is the same trap with the clear bit set */
    M.d[0]=32; m68k_trap(0xA322);
    CHECK(M.a[0]!=0 && M.d[0]==0, "NewHandleClear should also dispatch");
}

static void draw_scene(void){
    { uint32_t r=rect(0,0,QD_W,QD_H); push32(r); TRAP(0xA8A3); }    /* EraseRect */
    { uint32_t r=rect(20,20,QD_W-20,QD_H-20); push32(r); TRAP(0xA8A1); }
    push16(QD_W/2); push16(20); TRAP(0xA893);                        /* MoveTo */
    push16(QD_W/2); push16(QD_H-20); TRAP(0xA891);                   /* LineTo */
    { uint32_t r=rect(60,150,110,192); push32(r); TRAP(0xA8A2); }
    { uint32_t r=rect(QD_W-110,150,QD_W-60,192); push32(r); TRAP(0xA8A2); }
    { uint32_t r=rect(240,160,272,192); push32(0); push32(r); TRAP(0xA8BB); }
    push16(40); push16(40); TRAP(0xA893);
    { push32(pstr("SHUFFLEPUK")); TRAP(0xA884); }                    /* DrawString */

    FILE *f=fopen("hal_out.pgm","wb");
    fprintf(f,"P5\n%d %d\n255\n",QD_W,QD_H);
    for(int y=0;y<QD_H;y++) for(int x=0;x<QD_W;x++) fputc(qd_fb[y][x]?0:255,f);
    fclose(f);
}


/* TextEdit. The TERec lives in guest memory, so these assert the fields the
 * guest actually reads back -- teLength, selStart/selEnd, nLines, hText -- not
 * just that the traps return without complaining. */
#define TE_SELSTART 32
#define TE_SELEND   34
#define TE_LENGTH   60
#define TE_HTEXT    62
#define TE_NLINES   94

static uint32_t te_make(int w, int h){
    uint32_t d = rect(0,0,w,h), v = rect(0,0,w,h);
    call_begin(4); push32(d); push32(v); TRAP(0xA9D2);   /* TENew */
    return call_end32();
}
static void te_settext(uint32_t hTE, const char *t){
    int n = (int)strlen(t); uint32_t p = alloc(n+1);
    for(int i=0;i<n;i++) m68k_w8(p+i,(uint8_t)t[i]);
    push32(p); push32((uint32_t)n); push32(hTE); TRAP(0xA9CF);   /* TESetText */
}
static void test_textedit(void){
    printf("TextEdit\n");
    uint32_t hTE = te_make(200, 80);
    CHECK(hTE != 0, "TENew should return a TEHandle");
    uint32_t rec = m68k_r32(hTE);
    CHECK(rec != 0, "the TEHandle should dereference to a TERec");
    CHECK(m68k_r32(rec+TE_HTEXT) != 0, "a fresh TERec still needs an hText handle");

    te_settext(hTE, "hello");
    CHECK(rd16(rec+TE_LENGTH) == 5, "teLength should be 5, got %d", rd16(rec+TE_LENGTH));
    CHECK(rd16(rec+TE_NLINES) == 1, "one short line, got %d", rd16(rec+TE_NLINES));

    /* a CR starts a new line */
    te_settext(hTE, "ab\rcd");
    CHECK(rd16(rec+TE_LENGTH) == 5, "teLength across a CR");
    CHECK(rd16(rec+TE_NLINES) == 2, "a CR should split into 2 lines, got %d", rd16(rec+TE_NLINES));

    /* TESetSelect clamps to the text, and a reversed range is normalised */
    push32(1); push32(3); push32(hTE); TRAP(0xA9D1);
    CHECK(rd16(rec+TE_SELSTART)==1 && rd16(rec+TE_SELEND)==3, "selection 1..3");
    push32(99); push32(99); push32(hTE); TRAP(0xA9D1);
    CHECK(rd16(rec+TE_SELSTART)==5, "selection clamps to teLength, got %d", rd16(rec+TE_SELSTART));

    /* typing at the selection inserts and advances the caret */
    push32(2); push32(2); push32(hTE); TRAP(0xA9D1);
    push16('X'); push32(hTE); TRAP(0xA9DC);                       /* TEKey */
    CHECK(rd16(rec+TE_LENGTH)==6, "TEKey should insert one char, got %d", rd16(rec+TE_LENGTH));
    CHECK(rd16(rec+TE_SELSTART)==3, "caret should advance to 3, got %d", rd16(rec+TE_SELSTART));
    uint32_t tp = m68k_r32(m68k_r32(rec+TE_HTEXT));
    CHECK(m68k_r8(tp+2)=='X', "the inserted char should land at offset 2");

    /* backspace removes it again */
    push16(8); push32(hTE); TRAP(0xA9DC);
    CHECK(rd16(rec+TE_LENGTH)==5, "backspace should delete one char, got %d", rd16(rec+TE_LENGTH));

    /* cut then paste round-trips the selection */
    push32(0); push32(2); push32(hTE); TRAP(0xA9D1);
    push32(hTE); TRAP(0xA9D6);                                    /* TECut */
    CHECK(rd16(rec+TE_LENGTH)==3, "cut of 2 chars leaves 3, got %d", rd16(rec+TE_LENGTH));
    push32(hTE); TRAP(0xA9DB);                                    /* TEPaste */
    CHECK(rd16(rec+TE_LENGTH)==5, "paste restores to 5, got %d", rd16(rec+TE_LENGTH));

    /* TEUpdate must not fault on a live record */
    uint32_t up = rect(0,0,200,80);
    push32(up); push32(hTE); TRAP(0xA9D3);
}


/* File Manager. Register a real file on the host, then drive the traps the way
 * lifted code does -- A0 is the parameter block, D0 the result -- and check
 * both the bytes that come back and the error a missing file must report. */
#define PB_IORESULT    16
#define PB_IONAMEPTR   18
#define PB_IOREFNUM    24
#define PB_IOMISC      28
#define PB_IOBUFFER    32
#define PB_IOREQCOUNT  36
#define PB_IOACTCOUNT  40
#define PB_IOPOSMODE   44
#define PB_IOPOSOFFSET 46

static uint32_t fs_pb(const char *name){
    uint32_t pb = alloc(80);
    for(int i=0;i<80;i++) m68k_w8(pb+i,0);
    m68k_w32(pb+PB_IONAMEPTR, pstr(name));
    return pb;
}
static int fs_call(uint16_t trap, uint32_t pb){
    M.a[0]=pb; TRAP(trap); return (int16_t)(M.d[0]&0xFFFF);
}
static void test_files(void){
    printf("File Manager\n");
    const char *path = "hal_fs_test.tmp";
    FILE *f = fopen(path,"wb");
    CHECK(f!=NULL, "could not create the test file");
    if(!f) return;
    fwrite("ABCDEFGHIJ",1,10,f); fclose(f);
    fs_add("Scratch","TEXT","MACA", path, 10, "", 0);

    /* a file that is not there reports fnfErr, not silence */
    uint32_t pb = fs_pb("Nothing");
    CHECK(fs_call(0xA000,pb)==-43, "a missing file should be fnfErr, got %d",
          (int16_t)(M.d[0]&0xFFFF));

    pb = fs_pb("Scratch");
    CHECK(fs_call(0xA000,pb)==0, "Open should succeed");
    int ref = (int16_t)m68k_r16(pb+PB_IOREFNUM);
    CHECK(ref>0, "Open should hand back a refNum, got %d", ref);

    /* GetEOF reports the fork length */
    CHECK(fs_call(0xA011,pb)==0, "GetEOF should succeed");
    CHECK((int)m68k_r32(pb+PB_IOMISC)==10, "GetEOF should say 10, got %u",
          m68k_r32(pb+PB_IOMISC));

    /* a short read from the mark, then the mark has moved */
    uint32_t buf = alloc(32);
    m68k_w32(pb+PB_IOBUFFER, buf);
    m68k_w32(pb+PB_IOREQCOUNT, 4);
    m68k_w16(pb+PB_IOPOSMODE, 0);
    CHECK(fs_call(0xA002,pb)==0, "Read of 4 bytes should succeed");
    CHECK(m68k_r32(pb+PB_IOACTCOUNT)==4, "should have read 4");
    CHECK(m68k_r8(buf)=='A' && m68k_r8(buf+3)=='D', "should have read ABCD");
    CHECK(fs_call(0xA018,pb)==0 && m68k_r32(pb+PB_IOPOSOFFSET)==4,
          "the mark should be at 4, got %u", m68k_r32(pb+PB_IOPOSOFFSET));

    /* seek absolute, then read across the end: short count and eofErr */
    m68k_w16(pb+PB_IOPOSMODE,1); m68k_w32(pb+PB_IOPOSOFFSET,8);
    CHECK(fs_call(0xA044,pb)==0, "SetFPos to 8 should succeed");
    m68k_w32(pb+PB_IOREQCOUNT, 8); m68k_w16(pb+PB_IOPOSMODE,0);
    CHECK(fs_call(0xA002,pb)==-39, "a read past the end should report eofErr");
    CHECK(m68k_r32(pb+PB_IOACTCOUNT)==2, "...but still deliver 2 bytes, got %u",
          m68k_r32(pb+PB_IOACTCOUNT));
    CHECK(m68k_r8(buf)=='I' && m68k_r8(buf+1)=='J', "the last two bytes are IJ");

    /* the media is read-only, and says so rather than pretending */
    CHECK(fs_call(0xA003,pb)==-61, "Write should report wrPermErr");
    CHECK(fs_call(0xA001,pb)==0, "Close should succeed");
    CHECK(fs_call(0xA002,pb)==-51, "a read after Close should report rfNumErr");
    remove(path);
}

int main(void){
    M.memsize = 8u*1024*1024; M.mem = calloc(1,M.memsize);
    SP = 0x100000;
    toolbox_init();

    test_pascal_results();
    test_regions();
    test_dialog();
    test_memory();
    test_textedit();
    test_files();
    draw_scene();

    if(g_fail){ printf("HAL selftest: %d check(s) FAILED\n", g_fail); return 1; }
    printf("HAL selftest: all checks passed; scene -> hal_out.pgm\n");
    return 0;
}

/* textedit.c - TextEdit over the QuickDraw HAL.
 *
 * HyperCard's fields are TextEdit, and a run stops dead the moment it asks
 * TENew for a TEHandle and gets nothing back: it spins allocating. This is the
 * package that unblocks it -- 22 traps over 81 call sites in HyperCard 1.2.2.
 *
 * The TERec lives in **guest memory**, not in a host-side mirror. A TEHandle
 * dereferences to it, and the guest reads its fields directly (teLength,
 * selStart/selEnd, hText, nLines, lineStarts) -- so a parallel host table would
 * be a second copy to keep in sync for no gain. Everything here is therefore
 * read-modify-write through M.mem, which is also what the real Toolbox did.
 *
 * What this is not: styled text (TEStyleNew and friends), word-break and
 * click-loop hooks, or real scrolling. Layout is monospaced because the HAL's
 * font is, so line breaking is arithmetic rather than measurement.
 *
 * Record layout follows Inside Macintosh I-374; no Apple code is used. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include <string.h>

/* ---- TERec (Inside Macintosh I-374) ---- */
#define TE_DEST        0
#define TE_VIEW        8
#define TE_SELRECT    16
#define TE_LINEHEIGHT 24
#define TE_FONTASCENT 26
#define TE_SELPOINT   28
#define TE_SELSTART   32
#define TE_SELEND     34
#define TE_ACTIVE     36
#define TE_WORDBREAK  38
#define TE_CLIKLOOP   42
#define TE_CLICKTIME  46
#define TE_CLICKLOC   50
#define TE_CARETTIME  52
#define TE_CARETSTATE 56
#define TE_JUST       58
#define TE_LENGTH     60
#define TE_HTEXT      62
#define TE_RECALBACK  66
#define TE_RECALLINES 68
#define TE_CLIKSTUFF  70
#define TE_CRONLY     72
#define TE_TXFONT     74
#define TE_TXFACE     76
#define TE_TXMODE     78
#define TE_TXSIZE     80
#define TE_INPORT     82
#define TE_HIGHHOOK   86
#define TE_CARETHOOK  90
#define TE_NLINES     94
#define TE_LINESTARTS 96

#define TE_MAXLINES  512
#define TE_RECSIZE   (TE_LINESTARTS + 2 * (TE_MAXLINES + 1))

/* The HAL's font is fixed-pitch; these follow qd_draw_char's metrics. */
#define TE_CHARW      6
#define TE_ASCENT     7
#define TE_LINEH     10

static uint8_t g_scrap[4096];      /* TextEdit's own scrap (Cut/Copy/Paste) */
static int g_scraplen;

/* ---- small guest-memory helpers ---- */
static Rect te_rd_rect(uint32_t p){
    Rect r; r.top=(int16_t)m68k_r16(p); r.left=(int16_t)m68k_r16(p+2);
    r.bottom=(int16_t)m68k_r16(p+4); r.right=(int16_t)m68k_r16(p+6); return r;
}
static void te_wr_rect(uint32_t p, const Rect *r){
    m68k_w16(p,r->top); m68k_w16(p+2,r->left); m68k_w16(p+4,r->bottom); m68k_w16(p+6,r->right);
}
static uint32_t rec_of(uint32_t hTE){ return hTE ? m68k_r32(hTE) : 0; }
static uint32_t text_of(uint32_t rec){
    uint32_t h = m68k_r32(rec + TE_HTEXT);
    return h ? m68k_r32(h) : 0;
}
static int te_len(uint32_t rec){ return (int16_t)m68k_r16(rec + TE_LENGTH); }

/* ---- line breaking ---------------------------------------------------------
 * Break on CR, then wrap to the destRect width, preferring the last space on
 * the line. Monospaced, so a character count is a width. */
void te_calc(uint32_t hTE){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    uint32_t tp = text_of(rec);
    int len = te_len(rec);
    Rect dest = te_rd_rect(rec + TE_DEST);
    int cols = (dest.right - dest.left) / TE_CHARW;
    if(cols < 1) cols = 1;
    int crOnly = (int16_t)m68k_r16(rec + TE_CRONLY) != 0;

    int n = 0, i = 0;
    m68k_w16(rec + TE_LINESTARTS, 0);
    while(i < len && n < TE_MAXLINES){
        int start = i, brk = -1, taken = 0;
        while(i < len && taken < (crOnly ? len + 1 : cols)){
            uint8_t c = (uint8_t)m68k_r8(tp + i);
            i++; taken++;
            if(c == 13){ brk = i; break; }              /* CR ends the line */
            if(c == ' ') brk = i;
        }
        if(i < len && brk > start && (uint8_t)m68k_r8(tp + i - 1) != 13 && !crOnly)
            i = brk;                                     /* wrap at the space */
        n++;
        if(n <= TE_MAXLINES) m68k_w16(rec + TE_LINESTARTS + 2*n, i);
    }
    if(n == 0){ n = 1; m68k_w16(rec + TE_LINESTARTS + 2, 0); }
    m68k_w16(rec + TE_NLINES, n);
}

static int line_of(uint32_t rec, int off){
    int n = (int16_t)m68k_r16(rec + TE_NLINES);
    for(int i = n - 1; i >= 0; i--)
        if(off >= (int16_t)m68k_r16(rec + TE_LINESTARTS + 2*i)) return i;
    return 0;
}

/* ---- drawing --------------------------------------------------------------*/
void te_update(uint32_t hTE){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    uint32_t tp = text_of(rec);
    int len = te_len(rec), n = (int16_t)m68k_r16(rec + TE_NLINES);
    Rect dest = te_rd_rect(rec + TE_DEST), view = te_rd_rect(rec + TE_VIEW);
    int lh = (int16_t)m68k_r16(rec + TE_LINEHEIGHT); if(lh <= 0) lh = TE_LINEH;
    int asc = (int16_t)m68k_r16(rec + TE_FONTASCENT); if(asc <= 0) asc = TE_ASCENT;

    Rect save; qd_get_clip(&save);
    qd_set_clip(&view);
    qd_erase_rect(&view);
    for(int i = 0; i < n; i++){
        int a = (int16_t)m68k_r16(rec + TE_LINESTARTS + 2*i);
        int b = (i + 1 <= n) ? (int16_t)m68k_r16(rec + TE_LINESTARTS + 2*(i+1)) : len;
        if(b > len) b = len;
        if(b <= a) continue;
        int nch = b - a;
        while(nch > 0){                                  /* do not draw the CR */
            uint8_t c = (uint8_t)m68k_r8(tp + a + nch - 1);
            if(c == 13 || c == 10) nch--; else break;
        }
        int y = dest.top + i*lh + asc;
        if(y - asc > view.bottom) break;
        qd_pen_to(dest.left, y);
        for(int k = 0; k < nch; k++) qd_draw_char(m68k_r8(tp + a + k));
    }
    /* caret: only when active, and only on the "on" half of the blink */
    if((int16_t)m68k_r16(rec + TE_ACTIVE) && (int16_t)m68k_r16(rec + TE_CARETSTATE)){
        int sel = (int16_t)m68k_r16(rec + TE_SELSTART);
        int li = line_of(rec, sel);
        int a = (int16_t)m68k_r16(rec + TE_LINESTARTS + 2*li);
        Rect c;
        c.left = dest.left + (sel - a)*TE_CHARW; c.right = c.left + 1;
        c.top = dest.top + li*lh; c.bottom = c.top + lh;
        qd_fill_rect(&c, 1);
    }
    qd_set_clip(&save);
}

/* ---- text storage ----------------------------------------------------------
 * The bump heap never frees, so every edit allocates a fresh buffer and
 * repoints hText rather than resizing in place.
 * ponytail: text edits leak their old buffer; give the Memory Manager a real
 * free list if a title ever edits enough text to matter. */
static void te_store(uint32_t rec, const uint8_t *src, int len){
    if(len < 0) len = 0;
    uint32_t h = m68k_r32(rec + TE_HTEXT);
    uint32_t p = mr_alloc((uint32_t)(len > 0 ? len : 1));
    for(int i = 0; i < len; i++) m68k_w8(p + i, src[i]);
    if(!h){ h = mr_alloc(4); m68k_w32(rec + TE_HTEXT, h); }
    m68k_w32(h, p);
    m68k_w16(rec + TE_LENGTH, (uint16_t)len);
}
static int te_read(uint32_t rec, uint8_t *out, int max){
    uint32_t tp = text_of(rec);
    int len = te_len(rec); if(len > max) len = max;
    for(int i = 0; i < len; i++) out[i] = (uint8_t)m68k_r8(tp + i);
    return len;
}

/* Replace [a,b) with `ins`, then reflow and fix the selection. */
static void te_splice(uint32_t hTE, int a, int b, const uint8_t *ins, int nins){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    static uint8_t buf[8192], out[8192];
    int len = te_read(rec, buf, (int)sizeof buf);
    if(a < 0) a = 0; if(b > len) b = len; if(b < a) b = a;
    int n = 0;
    for(int i = 0; i < a && n < (int)sizeof out; i++) out[n++] = buf[i];
    for(int i = 0; i < nins && n < (int)sizeof out; i++) out[n++] = ins[i];
    for(int i = b; i < len && n < (int)sizeof out; i++) out[n++] = buf[i];
    te_store(rec, out, n);
    int sel = a + nins;
    m68k_w16(rec + TE_SELSTART, (uint16_t)sel);
    m68k_w16(rec + TE_SELEND,   (uint16_t)sel);
    te_calc(hTE);
}

/* ---- the calls the trap layer dispatches to ------------------------------ */
uint32_t te_new(uint32_t destPtr, uint32_t viewPtr){
    uint32_t rec = mr_alloc(TE_RECSIZE);
    uint32_t h   = mr_alloc(4);
    if(!rec || !h) return 0;
    for(uint32_t i = 0; i < TE_RECSIZE; i++) m68k_w8(rec + i, 0);
    m68k_w32(h, rec);

    Rect d = destPtr ? te_rd_rect(destPtr) : (Rect){0,0,0,0};
    Rect v = viewPtr ? te_rd_rect(viewPtr) : d;
    te_wr_rect(rec + TE_DEST, &d);
    te_wr_rect(rec + TE_VIEW, &v);
    m68k_w16(rec + TE_LINEHEIGHT, TE_LINEH);
    m68k_w16(rec + TE_FONTASCENT, TE_ASCENT);
    m68k_w16(rec + TE_TXSIZE, 12);
    m68k_w16(rec + TE_NLINES, 1);
    /* An empty TERec still needs a text handle: the guest may ask for hText
     * before ever setting any text, and a null there reads as a dead field. */
    te_store(rec, (const uint8_t *)"", 0);
    te_calc(h);
    return h;
}
void te_dispose(uint32_t hTE){ (void)hTE; }   /* bump heap: nothing to give back */

void te_set_text(uint32_t hTE, uint32_t src, int len){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    static uint8_t buf[8192];
    if(len < 0) len = 0; if(len > (int)sizeof buf) len = (int)sizeof buf;
    for(int i = 0; i < len; i++) buf[i] = (uint8_t)m68k_r8(src + i);
    te_store(rec, buf, len);
    m68k_w16(rec + TE_SELSTART, 0); m68k_w16(rec + TE_SELEND, 0);
    te_calc(hTE);
}
uint32_t te_get_text(uint32_t hTE){
    uint32_t rec = rec_of(hTE); return rec ? m68k_r32(rec + TE_HTEXT) : 0;
}
void te_set_select(uint32_t hTE, int a, int b){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    int len = te_len(rec);
    if(a < 0) a = 0; if(a > len) a = len;
    if(b < 0) b = 0; if(b > len) b = len;
    if(b < a){ int t = a; a = b; b = t; }
    m68k_w16(rec + TE_SELSTART, (uint16_t)a);
    m68k_w16(rec + TE_SELEND,   (uint16_t)b);
}
void te_activate(uint32_t hTE, int on){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    m68k_w16(rec + TE_ACTIVE, (uint16_t)(on ? 1 : 0));
    m68k_w16(rec + TE_CARETSTATE, (uint16_t)(on ? 1 : 0));
}
void te_idle(uint32_t hTE){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    if(!(int16_t)m68k_r16(rec + TE_ACTIVE)) return;
    /* Blink off the Ticks low-order bits rather than a host timer, so a
     * headless run advances the caret exactly as a windowed one does. */
    uint32_t t = m68k_r32(0x16A);
    m68k_w16(rec + TE_CARETSTATE, (uint16_t)((t >> 5) & 1));
}
/* Point -> offset, monospaced: column is a division, line is a division. */
void te_click(uint32_t hTE, int h, int v, int extend){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    Rect dest = te_rd_rect(rec + TE_DEST);
    int lh = (int16_t)m68k_r16(rec + TE_LINEHEIGHT); if(lh <= 0) lh = TE_LINEH;
    int n = (int16_t)m68k_r16(rec + TE_NLINES), len = te_len(rec);
    int li = (v - dest.top) / lh;
    if(li < 0) li = 0; if(li >= n) li = n - 1;
    int a = (int16_t)m68k_r16(rec + TE_LINESTARTS + 2*li);
    int b = (li + 1 <= n) ? (int16_t)m68k_r16(rec + TE_LINESTARTS + 2*(li+1)) : len;
    if(b > len) b = len;
    int off = a + (h - dest.left + TE_CHARW/2) / TE_CHARW;
    if(off < a) off = a; if(off > b) off = b;
    if(extend) te_set_select(hTE, (int16_t)m68k_r16(rec + TE_SELSTART), off);
    else       te_set_select(hTE, off, off);
}
void te_key(uint32_t hTE, int ch){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    int a = (int16_t)m68k_r16(rec + TE_SELSTART), b = (int16_t)m68k_r16(rec + TE_SELEND);
    if(ch == 8){                                   /* backspace */
        if(a == b && a > 0) a--;
        te_splice(hTE, a, b, 0, 0);
    } else {
        uint8_t c = (uint8_t)ch;
        te_splice(hTE, a, b, &c, 1);
    }
}
void te_insert(uint32_t hTE, uint32_t src, int len){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    static uint8_t buf[8192];
    if(len < 0) len = 0; if(len > (int)sizeof buf) len = (int)sizeof buf;
    for(int i = 0; i < len; i++) buf[i] = (uint8_t)m68k_r8(src + i);
    int a = (int16_t)m68k_r16(rec + TE_SELSTART);
    te_splice(hTE, a, a, buf, len);
}
void te_delete(uint32_t hTE){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    te_splice(hTE, (int16_t)m68k_r16(rec + TE_SELSTART),
                   (int16_t)m68k_r16(rec + TE_SELEND), 0, 0);
}
void te_copy(uint32_t hTE){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    static uint8_t buf[8192];
    int len = te_read(rec, buf, (int)sizeof buf);
    int a = (int16_t)m68k_r16(rec + TE_SELSTART), b = (int16_t)m68k_r16(rec + TE_SELEND);
    if(a < 0) a = 0; if(b > len) b = len;
    g_scraplen = 0;
    for(int i = a; i < b && g_scraplen < (int)sizeof g_scrap; i++) g_scrap[g_scraplen++] = buf[i];
}
void te_cut(uint32_t hTE){ te_copy(hTE); te_delete(hTE); }
void te_paste(uint32_t hTE){
    uint32_t rec = rec_of(hTE); if(!rec) return;
    te_splice(hTE, (int16_t)m68k_r16(rec + TE_SELSTART),
                   (int16_t)m68k_r16(rec + TE_SELEND), g_scrap, g_scraplen);
}
/* TETextBox: draw a string into a rect with no TERec at all. */
void te_text_box(uint32_t src, int len, const Rect *box, int just){
    (void)just;
    Rect save; qd_get_clip(&save);
    qd_set_clip(box);
    qd_erase_rect(box);
    int cols = (box->right - box->left) / TE_CHARW; if(cols < 1) cols = 1;
    int y = box->top + TE_ASCENT, i = 0;
    while(i < len && y < box->bottom + TE_LINEH){
        int n = 0;
        qd_pen_to(box->left, y);
        while(i < len && n < cols){
            uint8_t c = (uint8_t)m68k_r8(src + i); i++;
            if(c == 13){ n = cols; break; }
            qd_draw_char(c); n++;
        }
        y += TE_LINEH;
    }
    qd_set_clip(&save);
}

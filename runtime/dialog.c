/* dialog.c - Dialog Manager + Control Manager over the QuickDraw HAL.
 *
 * The Dialog Manager is the cheapest large win in a Toolbox HAL: a handful of
 * traps cover hundreds of call sites, because every alert, every settings pane
 * and every "which card?" prompt routes through GetNewDialog/ModalDialog.
 *
 * A dialog is a DITL resource (the item list) plus a window. We parse the DITL
 * once into a host-side item table rather than rewriting the variable-length
 * resource in guest memory on every SetDialogItemText. The guest sees a
 * DialogPtr and, per item, a real Handle: a ControlRecord for control items, a
 * Str255 buffer for text items. That is what GetDialogItem hands back and what
 * GetDialogItemText and SetControlValue are then called on.
 *
 * Record layouts follow Inside Macintosh; no Apple code is used. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---- DITL item types (Inside Macintosh I) ---- */
#define IT_USER      0
#define IT_BTN       4
#define IT_CHK       5
#define IT_RAD       6
#define IT_RESCTRL   7
#define IT_STATTEXT  8
#define IT_EDITTEXT 16
#define IT_ICON     32
#define IT_PICT     64
#define IT_DISABLE 128   /* OR'd into the type byte */

#define MAX_ITEMS  64
#define MAX_DLGS   16
#define MAX_TEXT  256

/* ControlRecord (Inside Macintosh I-319) */
#define CTL_RECT      8
#define CTL_VIS      16
#define CTL_HILITE   17
#define CTL_VALUE    18
#define CTL_MIN      20
#define CTL_MAX      22
#define CTL_TITLE    40
#define CTL_SIZE    300

/* Per item the arena holds: a 4-byte handle cell, then the record it points at
 * (a ControlRecord, or a Str255 for text items). */
#define ITEM_SLOT (4 + CTL_SIZE)

/* DialogRecord: a WindowRecord (156 bytes) then the dialog fields. */
#define DLG_ITEMS     156
#define DLG_DEFITEM   168

typedef struct {
    int      type;            /* with the disable bit stripped */
    int      enabled, hidden;
    Rect     box;
    uint8_t  text[MAX_TEXT];  /* Pascal string: text[0] = length */
    uint32_t hnd;             /* guest Handle for this item (0 if none) */
    uint32_t rec;             /* what hnd points at */
    int16_t  res_id;          /* icon / picture items */
} Item;

typedef struct {
    int      used;
    uint32_t dlg;             /* guest DialogPtr: the key */
    int      n, def_item;
    Rect     bounds;          /* where the window sits on screen */
    int      framed;          /* draw the window frame (alerts and dialogs) */
    Item     it[MAX_ITEMS];
} Dlg;

static Dlg g_dlg[MAX_DLGS];
static uint8_t g_param[4][MAX_TEXT];      /* ParamText ^0..^3 */

static Rect rd_r(uint32_t p){
    Rect r; r.top=(int16_t)m68k_r16(p); r.left=(int16_t)m68k_r16(p+2);
    r.bottom=(int16_t)m68k_r16(p+4); r.right=(int16_t)m68k_r16(p+6); return r;
}
static void wr_r(uint32_t p, const Rect *r){
    m68k_w16(p,r->top); m68k_w16(p+2,r->left); m68k_w16(p+4,r->bottom); m68k_w16(p+6,r->right);
}
static void pstr_get(uint8_t *dst, uint32_t p, int max){
    int n = p ? (int)m68k_r8(p) : 0; if(n > max-2) n = max-2;
    dst[0]=(uint8_t)n; for(int i=0;i<n;i++) dst[i+1]=(uint8_t)m68k_r8(p+1+i);
}
static void pstr_put(uint32_t p, const uint8_t *s){
    if(!p) return; int n=s[0]; m68k_w8(p,n); for(int i=0;i<n;i++) m68k_w8(p+1+i,s[i+1]);
}
static Dlg *find(uint32_t d){
    for(int i=0;i<MAX_DLGS;i++) if(g_dlg[i].used && g_dlg[i].dlg==d) return &g_dlg[i];
    return NULL;
}
static Item *by_handle(uint32_t h){
    if(!h) return NULL;
    for(int i=0;i<MAX_DLGS;i++){ if(!g_dlg[i].used) continue;
        for(int k=0;k<g_dlg[i].n;k++) if(g_dlg[i].it[k].hnd==h) return &g_dlg[i].it[k]; }
    return NULL;
}

static void origin_of(const Dlg *d, int *dh, int *dv){
    *dh = d->framed ? d->bounds.left : 0;
    *dv = d->framed ? d->bounds.top  : 0;
}

/* The dialog that owns this control, for the drawing origin. */
static Dlg *owner_of(uint32_t ctl){
    if(!ctl) return NULL;
    return find(m68k_r32(ctl + 4));            /* ControlRecord.contrlOwner */
}

/* ParamText: expand ^0..^3 at draw/read time, so a ParamText call still affects
 * a dialog that already exists -- which is how real code uses it. */
static void subst(uint8_t *out, const uint8_t *in, int max){
    int n=in[0], o=0;
    for(int i=1;i<=n && o<max-2;i++){
        if(in[i]=='^' && i<n && in[i+1]>='0' && in[i+1]<='3'){
            const uint8_t *p=g_param[in[i+1]-'0'];
            for(int k=1;k<=p[0] && o<max-2;k++) out[++o]=p[k];
            i++;
        } else out[++o]=in[i];
    }
    out[0]=(uint8_t)o;
}

/* ---- controls ---- */
uint32_t ctl_new(uint32_t owner, const Rect *box, const uint8_t *title,
                 int vis, int value, int min, int max, uint32_t rec){
    if(!rec) return 0;
    m68k_w32(rec,0); m68k_w32(rec+4,owner);
    wr_r(rec+CTL_RECT, box);
    m68k_w8(rec+CTL_VIS, vis?0xFF:0); m68k_w8(rec+CTL_HILITE,0);
    m68k_w16(rec+CTL_VALUE,value); m68k_w16(rec+CTL_MIN,min); m68k_w16(rec+CTL_MAX,max);
    if(title) pstr_put(rec+CTL_TITLE, title);
    return rec;
}
int  ctl_value(uint32_t c){ return c ? (int16_t)m68k_r16(c+CTL_VALUE) : 0; }
void ctl_set_value(uint32_t c, int v){ if(c) m68k_w16(c+CTL_VALUE, v); }
void ctl_set_hilite(uint32_t c, int h){ if(c) m68k_w8(c+CTL_HILITE, h); }
Rect ctl_rect(uint32_t c){ Rect r={0,0,0,0}; if(c) r=rd_r(c+CTL_RECT); return r; }

void ctl_draw(uint32_t c){
    int _wasport = qd_port_to_screen();
    if(!c || !m68k_r8(c+CTL_VIS)){ qd_port_restore(_wasport); return; }
    Rect r = rd_r(c+CTL_RECT);
    Dlg *o = owner_of(c);
    if(o){ int dh,dv; origin_of(o,&dh,&dv); rect_offset(&r,dh,dv); }
    qd_frame_rect(&r);
    uint8_t t[MAX_TEXT]; pstr_get(t, c+CTL_TITLE, MAX_TEXT);
    if(t[0]){
        qd_pen_to((r.left+r.right)/2 - qd_text_width(t[0])/2, (r.top+r.bottom)/2 + 4);
        qd_draw_text(t+1, t[0]);
    }
    if(m68k_r8(c+CTL_HILITE)==1) qd_invert_rect(&r);   /* pressed */
    qd_port_restore(_wasport);
}

/* ---- dialogs ---- */
/* DITL item: 4-byte placeholder, 8-byte rect, type byte, length byte,
 * `length` data bytes, padded to an even boundary. */
static void parse_ditl(Dlg *d, uint32_t ditl, uint32_t arena, uint32_t arena_end){
    d->n = 0;
    if(!ditl) return;
    uint32_t p = ditl;
    int count = (int)(int16_t)m68k_r16(p) + 1; p += 2;
    if(count < 0) count = 0;
    if(count > MAX_ITEMS) count = MAX_ITEMS;
    for(int i=0;i<count;i++){
        Item *it = &d->it[d->n];
        memset(it, 0, sizeof *it);
        p += 4;
        it->box = rd_r(p); p += 8;
        int type = (int)m68k_r8(p++);
        int len  = (int)m68k_r8(p++);
        it->enabled = (type & IT_DISABLE) ? 0 : 1;
        it->type    = type & ~IT_DISABLE;
        if(it->type==IT_ICON || it->type==IT_PICT){
            it->res_id = (int16_t)m68k_r16(p);
        } else {
            int n = len > MAX_TEXT-2 ? MAX_TEXT-2 : len;
            it->text[0] = (uint8_t)n;
            for(int k=0;k<n;k++) it->text[k+1] = (uint8_t)m68k_r8(p+k);
        }
        p += len; if(len & 1) p++;

        if(arena + ITEM_SLOT <= arena_end){
            it->hnd = arena; it->rec = arena + 4;
            m68k_w32(it->hnd, it->rec);
            arena += ITEM_SLOT;
            if(it->type>=IT_BTN && it->type<=IT_RESCTRL)
                ctl_new(d->dlg, &it->box, it->text, !it->hidden, 0, 0, 1, it->rec);
            else
                pstr_put(it->rec, it->text);
        }
        d->n++;
    }
}

uint32_t dlg_new(uint32_t dlgptr, uint32_t ditl, uint32_t arena, uint32_t arena_end){
    Dlg *d = NULL;
    for(int i=0;i<MAX_DLGS;i++) if(!g_dlg[i].used){ d=&g_dlg[i]; break; }
    if(!d) return 0;
    memset(d,0,sizeof *d);
    d->used=1; d->dlg=dlgptr; d->def_item=1;
    parse_ditl(d, ditl, arena, arena_end);
    m68k_w32(dlgptr + DLG_ITEMS, ditl);
    m68k_w16(dlgptr + DLG_DEFITEM, 1);
    return dlgptr;
}

/* Item boxes stay in window-local coordinates, which is what GetDialogItem is
 * defined to return. The window origin is applied when drawing and hit-testing
 * instead -- the real Dialog Manager gets this from the port origin. */
void dlg_set_bounds(uint32_t dlgptr, const Rect *b){
    Dlg *d=find(dlgptr); if(!d||!b) return;
    d->bounds=*b; d->framed=1;
}

/* qd_fb is an overlay: a set pixel wins over the title's own screen memory,
 * so whatever a dialog painted there keeps hiding the card until it is
 * cleared. Erasing the rect is the restore -- a clear pixel falls through to
 * what the title drew underneath, which is still intact. */
void dlg_dispose(uint32_t dlgptr){
    Dlg *d = find(dlgptr);
    if(!d) return;
    if(d->framed){
        int was = qd_port_to_screen();
        Rect r = d->bounds; r.right += 3; r.bottom += 3;   /* include the shadow */
        Rect clip; qd_get_clip(&clip);
        Rect all; rect_set(&all, 0, 0, QD_W, QD_H); qd_set_clip(&all);
        qd_erase_rect(&r);
        qd_set_clip(&clip);
        qd_port_restore(was);
    }
    d->used = 0;
}
int  dlg_count(uint32_t dlgptr){ Dlg *d=find(dlgptr); return d?d->n:0; }

int dlg_get_item(uint32_t dlgptr, int n, int *type, uint32_t *h, Rect *box){
    Dlg *d=find(dlgptr);
    if(!d || n<1 || n>d->n) return 0;
    Item *it=&d->it[n-1];
    if(type) *type = it->type | (it->enabled?0:IT_DISABLE);
    if(h)    *h    = it->hnd;
    if(box)  *box  = it->box;
    return 1;
}
void dlg_set_item_rect(uint32_t dlgptr, int n, const Rect *box){
    Dlg *d=find(dlgptr); if(!d||n<1||n>d->n||!box) return;
    d->it[n-1].box=*box;
    if(d->it[n-1].type>=IT_BTN && d->it[n-1].type<=IT_RESCTRL && d->it[n-1].rec)
        wr_r(d->it[n-1].rec + CTL_RECT, box);
}
void dlg_hide_item(uint32_t dlgptr, int n, int hide){
    Dlg *d=find(dlgptr); if(!d||n<1||n>d->n) return;
    Item *it=&d->it[n-1]; it->hidden=hide;
    if(it->type>=IT_BTN && it->type<=IT_RESCTRL && it->rec)
        m68k_w8(it->rec + CTL_VIS, hide?0:0xFF);
}
void dlg_param_text(uint32_t p0,uint32_t p1,uint32_t p2,uint32_t p3){
    uint32_t ps[4]={p0,p1,p2,p3};
    for(int i=0;i<4;i++) if(ps[i]) pstr_get(g_param[i], ps[i], MAX_TEXT);
}

/* GetDialogItemText / SetDialogItemText take the item Handle, not dialog+index. */
void dlg_get_text_h(uint32_t h, uint32_t out){
    Item *it = by_handle(h);
    if(!it){ if(out) m68k_w8(out,0); return; }
    uint8_t buf[MAX_TEXT]; subst(buf, it->text, MAX_TEXT); pstr_put(out, buf);
}
void dlg_set_text_h(uint32_t h, uint32_t pstr){
    Item *it = by_handle(h); if(!it) return;
    pstr_get(it->text, pstr, MAX_TEXT);
    if(it->rec){
        if(it->type>=IT_BTN && it->type<=IT_RESCTRL) pstr_put(it->rec + CTL_TITLE, it->text);
        else pstr_put(it->rec, it->text);
    }
}

/* FindDialogItem returns a 0-based index, or -1 for none (Inside Macintosh). */
int dlg_find_item(uint32_t dlgptr, int h, int v){
    Dlg *d=find(dlgptr); if(!d) return -1;
    int dh,dv; origin_of(d,&dh,&dv);
    for(int i=0;i<d->n;i++){
        if(d->it[i].hidden) continue;
        Rect r=d->it[i].box; rect_offset(&r,dh,dv);
        if(pt_in_rect(h,v,&r)) return i;
    }
    return -1;
}

void dlg_draw(uint32_t dlgptr){
    int _wasport = qd_port_to_screen();
    Dlg *d=find(dlgptr); if(!d){ qd_port_restore(_wasport); return; }
    if(d->framed){                       /* white card, black border, drop shadow */
        Rect sh=d->bounds; rect_offset(&sh,3,3); qd_paint_rect(&sh);
        qd_erase_rect(&d->bounds);
        qd_frame_rect(&d->bounds);
        Rect in=d->bounds; rect_inset(&in,2,2); qd_frame_rect(&in);
    }
    int dh,dv; origin_of(d,&dh,&dv);
    for(int i=0;i<d->n;i++){
        Item *it=&d->it[i];
        if(it->hidden) continue;
        if(it->type>=IT_BTN && it->type<=IT_RESCTRL){ ctl_draw(it->rec); continue; }
        Rect box=it->box; rect_offset(&box,dh,dv);
        if(it->type==IT_EDITTEXT){ Rect fr=box; rect_inset(&fr,-3,-3); qd_frame_rect(&fr); }
        if(it->type==IT_EDITTEXT || it->type==IT_STATTEXT){
            uint8_t buf[MAX_TEXT]; subst(buf, it->text, MAX_TEXT);
            /* MRDLG=1: what the dialog actually says. A modal alert with no
             * one to read it is the whole reason a run stops here. */
            if(getenv("MRDLG")) fprintf(stderr, "[dlg] %.*s\n", buf[0], buf+1);
            qd_pen_to(box.left, box.top + 10);
            qd_draw_text(buf+1, buf[0]);
        }
    }
    /* The default item wears a heavy outline; one extra frame is close enough. */
    if(d->def_item>=1 && d->def_item<=d->n){
        Rect r=d->it[d->def_item-1].box; rect_offset(&r,dh,dv);
        rect_inset(&r,-4,-4); qd_frame_rect(&r);
    }
    qd_port_restore(_wasport);
}

/* ModalDialog: pump until an enabled item is hit, or Return/Enter.
 *
 * ponytail: no filterProc, no TextEdit caret, no double-click. The filterProc is
 * a guest callback, so wiring it needs m68k_call, not a C pointer -- do that
 * when a title turns up that depends on it. */
int dlg_modal(uint32_t dlgptr){
    Dlg *d=find(dlgptr); if(!d) return 0;
    dlg_draw(dlgptr);
    for(;;){
        plat_present();
        if(plat_quit_requested()) return d->def_item;
        plat_pump();
        int what=0,msg=0,h=0,v=0;
        if(!plat_next_event(&what,&msg,&h,&v)) continue;
        if(what==1){                                  /* mouseDown */
            int mh,mv; plat_get_mouse(&mh,&mv);
            int idx=dlg_find_item(dlgptr,mh,mv);
            if(idx>=0 && d->it[idx].enabled){
                Item *it=&d->it[idx];
                if(it->type>=IT_BTN && it->type<=IT_RESCTRL){
                    ctl_set_hilite(it->rec,1); ctl_draw(it->rec); plat_present();
                    ctl_set_hilite(it->rec,0);
                    if(it->type==IT_CHK) ctl_set_value(it->rec, !ctl_value(it->rec));
                }
                return idx+1;
            }
        } else if(what==3 || what==5){                /* keyDown / autoKey */
            int ch = msg & 0xFF;
            if(ch==13 || ch==3) return d->def_item;
        }
    }
}

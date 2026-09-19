/* toolbox.h - macrecomp Mac Toolbox HAL (QuickDraw/Event/... -> SDL2).
 *
 * The lifted code calls m68k_trap($Axxx); toolbox.c routes it here. QuickDraw
 * draws into a classic 512x342 1-bit framebuffer that platform_sdl.c presents
 * (scaled, black-on-white) in an SDL2 window. Geometry mirrors Inside Macintosh;
 * no Apple code is used. */
#ifndef MACRECOMP_TOOLBOX_H
#define MACRECOMP_TOOLBOX_H

#include <stdint.h>

#define QD_W 512
#define QD_H 342

void toolbox_init(void);   /* qd_init + heap; call once after M.mem is allocated */
void res_add(const char *type, int id, const uint8_t *data, int len);  /* register a resource */
/* Resources belonging to an opened file rather than to the application. A
 * document carries its own, and a title expects the one it just opened to be
 * searched first. refnum 1 is the application itself. */
void res_add_file(int refnum, const char *type, int id, const uint8_t *data, int len);
void res_use_file(int refnum);
int  res_cur_file(void);

typedef struct { int16_t top, left, bottom, right; } Rect;   /* Mac field order */
typedef struct { int16_t v, h; } Point;

/* ---- QuickDraw (quickdraw.c) ---- */
extern uint8_t qd_fb[QD_H][QD_W];       /* 1 byte/pixel, 0=white 1=black (simple) */
void qd_init(void);
void qd_pen_to(int h, int v);
void qd_get_pen(int *h, int *v);
/* Base of the 1-bit screen block in guest memory. QuickDraw draws into qd_fb,
 * but a title that blits with its own code writes here, so the presented frame
 * is the union of the two. */
uint32_t mr_screen_base(void);
/* Queue a synthetic click (MRCLICK); driven by the HAL's event loop. */
void plat_inject_click(int x, int y);
void qd_line_to(int h, int v);
void qd_line(int dh, int dv);
void qd_pen_size(int w, int h);
void qd_pen_mode(int mode);
void qd_pen_pat_black(int black);       /* 1 = black pattern, 0 = white */
void qd_frame_rect(const Rect *r);
void qd_paint_rect(const Rect *r);      /* fill with pen pattern */
void qd_erase_rect(const Rect *r);      /* fill white */
void qd_invert_rect(const Rect *r);
void qd_fill_rect(const Rect *r, int black);
void qd_frame_oval(const Rect *r);
void qd_fill_oval(const Rect *r, int black);
void qd_draw_char(int c);
int  qd_text_width(int len);   /* width of len chars in the current font */
void qd_draw_text(const uint8_t *p, int len);
/* CopyBits: blit a 1-bit source into the framebuffer (core of the game's art). */
void qd_copybits(const uint8_t *src, int src_rowbytes, int sw, int sh,
                 const Rect *srcR, const Rect *dstR, int mode);
void qd_set_clip(const Rect *r);
void qd_get_clip(Rect *r);
/* set the current drawing target: the screen (is_screen=1) or a 1-bit BitMap in
 * guest memory (base/rowbytes and the bitmap bounds' left/top origin) */
void qd_set_port(int is_screen, uint32_t base, int rowbytes,
                 int bl, int bt, int br, int bb);

/* rect utilities */
void rect_set(Rect *r, int l, int t, int rt, int b);
void rect_offset(Rect *r, int dh, int dv);
void rect_inset(Rect *r, int dh, int dv);
int  rect_union(const Rect *a, const Rect *b, Rect *out);
int  rect_sect(const Rect *a, const Rect *b, Rect *out);
int  pt_in_rect(int h, int v, const Rect *r);

/* ---- Dialog + Control Manager (dialog.c) ----
 * Dialogs are parsed from their DITL into a host-side item table; the guest
 * sees a DialogPtr and, for control items, a real ControlRecord. */
uint32_t dlg_new(uint32_t dlgptr, uint32_t ditl, uint32_t arena, uint32_t arena_end);
void dlg_set_bounds(uint32_t dlgptr, const Rect *b);  /* place window + items */
void dlg_dispose(uint32_t dlgptr);
int  dlg_count(uint32_t dlgptr);
int  dlg_get_item(uint32_t dlgptr, int n, int *type, uint32_t *h, Rect *box);
void dlg_set_item_rect(uint32_t dlgptr, int n, const Rect *box);
void dlg_set_text_h(uint32_t itemHandle, uint32_t pstr);  /* SetDialogItemText */
void dlg_get_text_h(uint32_t itemHandle, uint32_t out);   /* GetDialogItemText */
void dlg_hide_item(uint32_t dlgptr, int n, int hide);
void dlg_param_text(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3);
int  dlg_find_item(uint32_t dlgptr, int h, int v);  /* 0-based, -1 = none */
void dlg_draw(uint32_t dlgptr);
int  dlg_modal(uint32_t dlgptr);                    /* 1-based item hit */

uint32_t ctl_new(uint32_t owner, const Rect *box, const uint8_t *title,
                 int vis, int value, int min, int max, uint32_t rec);
int  ctl_value(uint32_t c);
void ctl_set_value(uint32_t c, int v);
void ctl_set_hilite(uint32_t c, int h);
Rect ctl_rect(uint32_t c);
void ctl_draw(uint32_t c);

/* ---- platform (platform_sdl.c) ---- */
int  plat_open(const char *title, int scale);   /* create window; returns 0 ok */
void plat_present(void);                         /* push qd_fb -> screen */
void plat_pump(void);                            /* poll OS events into the queue */
int  plat_quit_requested(void);
void plat_get_mouse(int *h, int *v);
int  plat_button(void);
uint32_t plat_ticks(void);                        /* 1/60 s since start */
void plat_shutdown(void);
/* next event; fills a minimal EventRecord-ish; returns 1 if a real event */
int  plat_next_event(int *what, int *msg, int *modh, int *modv);

/* ---- allocation out of the guest heap (toolbox.c owns the bump heap) ---- */
uint32_t mr_alloc(uint32_t n);   /* 0 if the heap cannot satisfy it */

/* ---- File Manager (files.c). Register the files a title's media holds, then
 * the register-based File Manager traps serve them read-only. Forks are read
 * from the host on demand, so a CD-ROM's hundreds of megabytes stay on disk. */
void fs_add(const char *name, const char *type, const char *creator,
            const char *dpath, uint32_t dlen, const char *rpath, uint32_t rlen);
int  fs_trap(uint16_t w);        /* 1 if this trap was a File Manager call */
int  fs_dispatch(uint16_t w);    /* FSDispatch / HFSDispatch (selector in D0) */
int  fs_open_resfork(uint32_t namePtr);  /* a document's own resource fork */

/* ---- TextEdit (textedit.c). The TERec lives in guest memory; a TEHandle
 * dereferences to it, so the guest reads teLength/selStart/hText directly. ---- */
uint32_t te_new(uint32_t destPtr, uint32_t viewPtr);   /* -> TEHandle */
void     te_dispose(uint32_t hTE);
void     te_set_text(uint32_t hTE, uint32_t src, int len);
uint32_t te_get_text(uint32_t hTE);                    /* -> the hText Handle */
void     te_calc(uint32_t hTE);                        /* reflow line starts */
void     te_update(uint32_t hTE);                      /* draw text + caret */
void     te_set_select(uint32_t hTE, int a, int b);
void     te_activate(uint32_t hTE, int on);
void     te_idle(uint32_t hTE);                        /* blink the caret */
void     te_click(uint32_t hTE, int h, int v, int extend);
void     te_key(uint32_t hTE, int ch);
void     te_insert(uint32_t hTE, uint32_t src, int len);
void     te_delete(uint32_t hTE);
void     te_cut(uint32_t hTE);
void     te_copy(uint32_t hTE);
void     te_paste(uint32_t hTE);
void     te_text_box(uint32_t src, int len, const Rect *box, int just);

#endif /* MACRECOMP_TOOLBOX_H */

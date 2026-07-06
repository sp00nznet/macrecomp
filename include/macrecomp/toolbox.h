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

typedef struct { int16_t top, left, bottom, right; } Rect;   /* Mac field order */
typedef struct { int16_t v, h; } Point;

/* ---- QuickDraw (quickdraw.c) ---- */
extern uint8_t qd_fb[QD_H][QD_W];       /* 1 byte/pixel, 0=white 1=black (simple) */
void qd_init(void);
void qd_pen_to(int h, int v);
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
void qd_draw_text(const uint8_t *p, int len);
/* CopyBits: blit a 1-bit source into the framebuffer (core of the game's art). */
void qd_copybits(const uint8_t *src, int src_rowbytes, int sw, int sh,
                 const Rect *srcR, const Rect *dstR, int mode);
void qd_set_clip(const Rect *r);

/* rect utilities */
void rect_set(Rect *r, int l, int t, int rt, int b);
void rect_offset(Rect *r, int dh, int dv);
void rect_inset(Rect *r, int dh, int dv);
int  rect_union(const Rect *a, const Rect *b, Rect *out);
int  rect_sect(const Rect *a, const Rect *b, Rect *out);
int  pt_in_rect(int h, int v, const Rect *r);

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

#endif /* MACRECOMP_TOOLBOX_H */

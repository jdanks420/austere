#ifndef AUSTERE_DRAW_H
#define AUSTERE_DRAW_H

#include <stdint.h>
#include <xcb/xcb.h>

#include "wm.h"

/* Opaque freetype font handle: one per fontconfig pattern, cached on
 * wm->fonts. Glyph metrics come from the cached freetype face so
 * bar/panel drawing never round-trips. */
typedef struct font font_t;

typedef struct draw {
    xcb_window_t win;
    xcb_gcontext_t gc;
    uint32_t gc_fg;     /* last foreground programmed into gc */
    bool gc_fg_known;   /* gc_fg valid after the first change_gc */
    uint8_t depth;       /* drawable depth, queried at draw_setup */
} draw_t;

#define DRAW_DEFAULT_FONT "Agave Nerd Font Mono:pixelsize=16"

font_t *draw_font(wm_t *wm, const char *pattern);
font_t *draw_ui_font(wm_t *wm);
unsigned font_height(const font_t *f);
unsigned font_ascent(const font_t *f);
unsigned draw_text_w(wm_t *wm, const font_t *f, const char *s,
    unsigned len);
void draw_setup(wm_t *wm, draw_t *d, xcb_window_t win);
void draw_shutdown(wm_t *wm);
void draw_rect(wm_t *wm, draw_t *d, int x, int y, unsigned w,
    unsigned h, uint32_t pixel);
void draw_text(wm_t *wm, draw_t *d, const font_t *f, int x,
    int y_baseline, const char *s, unsigned len, uint32_t fg,
    uint32_t bg);
/* Fast one-shot image blit. ARGB32 host-order input is packed to the
 * given depth: depth-32 draws take it verbatim; depth-24 gets BGR
 * triples with 4-byte scanline padding so put_image never drifts.
 * `argb` is read-only. */
void draw_put_image24(wm_t *wm, xcb_window_t win, xcb_gcontext_t gc,
    uint8_t depth, int16_t x, int16_t y, unsigned w, unsigned h,
    const uint32_t *argb);

#endif

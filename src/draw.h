#ifndef AUSTERE_DRAW_H
#define AUSTERE_DRAW_H

#include <stdint.h>
#include <xcb/xcb.h>

#include "wm.h"

/* Core-font handle: loaded once per XLFD, cached on wm->fonts. Widths
 * come from the cached glyph metrics so bar/panel drawing never
 * round-trips to the server. */
typedef struct font font_t;

typedef struct draw {
    xcb_window_t win;
    xcb_gcontext_t gc;
} draw_t;

#define DRAW_DEFAULT_FONT "fixed"

font_t *draw_font(wm_t *wm, const char *xlfd);
font_t *draw_ui_font(wm_t *wm);
unsigned font_height(const font_t *f);
unsigned font_ascent(const font_t *f);
unsigned draw_text_w(wm_t *wm, const font_t *f, const char *s,
    unsigned len);
void draw_setup(wm_t *wm, draw_t *d, xcb_window_t win);
void draw_shutdown(wm_t *wm);
void draw_rect(wm_t *wm, const draw_t *d, int x, int y, unsigned w,
    unsigned h, uint32_t pixel);
void draw_text(wm_t *wm, const draw_t *d, const font_t *f, int x,
    int y_baseline, const char *s, unsigned len, uint32_t fg,
    uint32_t bg);

#endif

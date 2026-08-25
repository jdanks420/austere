#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "font_agave.h"
#include "settings.h"
#include "draw.h"
#include "util.h"

struct font {
    char name[128];
    xcb_font_t id;
    const builtin_font_t *builtin; /* non-NULL: embedded bitmap font */
    unsigned ascent;
    unsigned descent;
    xcb_charinfo_t *infos;
    unsigned n_infos;
    unsigned min_char;
    unsigned default_char;
    font_t *next;
};

/* Decode one UTF-8 sequence; advances *p, returns codepoint (0 on
 * malformed). */
static uint32_t
utf8_cp(const char **p, const char *end)
{
    const unsigned char *u = (const unsigned char *)(*p);
    uint32_t cp;
    unsigned n;

    if (*p >= end)
        return 0;
    if (!(u[0] & 0x80)) {
        (*p)++;
        return u[0];
    }
    if ((u[0] & 0xe0) == 0xc0)
        cp = u[0] & 0x1f, n = 1;
    else if ((u[0] & 0xf0) == 0xe0)
        cp = u[0] & 0x0f, n = 2;
    else if ((u[0] & 0xf8) == 0xf0)
        cp = u[0] & 0x07, n = 3;
    else {
        (*p)++;
        return 0;
    }
    (*p)++;
    while (n--) {
        unsigned char c;

        if (*p >= end)
            return 0;
        c = (unsigned char)*(*p)++;
        if ((c & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (c & 0x3f);
    }
    return cp;
}

/* Offset of an extended glyph in the bits array, or (size_t)-1. */
static size_t
ext_off(const builtin_font_t *b, uint32_t cp)
{
    unsigned lo = 0, hi = b->ext_n;

    if (!b->ext)
        return (size_t)-1;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;

        if (b->ext[mid].cp < cp)
            lo = mid + 1;
        else if (b->ext[mid].cp > cp)
            hi = mid;
        else
            return b->ext[mid].off;
    }
    return (size_t)-1;
}

/* Glyph advance in pixels from the cached metrics. */
static unsigned
char_w(const font_t *f, unsigned char c)
{
    const xcb_charinfo_t *ci;

    if (!f->infos)
        return 8;
    if (c < f->n_infos)
        ci = &f->infos[c];
    else
        ci = &f->infos[f->default_char < f->n_infos ? f->default_char
                                                    : 0];
    return (unsigned)ci->character_width;
}

/* The UI-wide font: "agave-NN" (embedded Agave Nerd, pixel size NN),
 * any other string = server core font XLFD, empty = embedded default. */
static bool
ci_has(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return true;
    return false;
}

static const builtin_font_t *
ui_select(void)
{
    /* font = pixel size digits ("18"), the embedded face in any human
     * spelling with optional size ("Agave Nerd Font", "agave mono 20"),
     * or a core font XLFD string. */
    const char *f = cfg.font;
    bool agave = f && ci_has(f, "agave");
    bool sized = f && strspn(f, "0123456789 ") == strlen(f) && *f;

    if (!f || !*f)
        return builtin_fonts[2];
    if (!agave && !sized)
        return NULL; /* server XLFD */

    long px = -1;

    if (sized) {
        px = strtol(f, NULL, 10);
    } else {
        const char *d = f;

        while (*d && (*d < '0' || *d > '9'))
            d++;
        if (*d)
            px = strtol(d, NULL, 10);
    }
    if (px < 0)
        return builtin_fonts[2];
    const builtin_font_t *best = builtin_fonts[0];

    for (unsigned i = 0; builtin_fonts[i]; i++)
        if (atoi(builtin_fonts[i]->name + 5) <= px)
            best = builtin_fonts[i];
    return best;
}

font_t *
draw_ui_font(wm_t *wm)
{
    static font_t builtin;
    const builtin_font_t *b = ui_select();

    if (b) {
        builtin.builtin = b;
        builtin.ascent = b->ascent;
        builtin.descent = b->descent;
        builtin.id = XCB_NONE;
        return &builtin;
    }
    return draw_font(wm, cfg.font && *cfg.font ? cfg.font : NULL);
}

font_t *
draw_font(wm_t *wm, const char *xlfd)
{
    if (!xlfd || !*xlfd)
        xlfd = DRAW_DEFAULT_FONT;

    for (font_t *f = wm->fonts; f; f = f->next)
        if (!strcmp(f->name, xlfd))
            return f;

    xcb_font_t id = xcb_generate_id(wm->conn);
    size_t len = strlen(xlfd);

    xcb_open_font(wm->conn, id, (uint16_t)len, xlfd);
    xcb_query_font_cookie_t ck = xcb_query_font(wm->conn, id);
    xcb_query_font_reply_t *r = xcb_query_font_reply(wm->conn, ck, NULL);
    font_t *f;

    if (!r) {
        /* Font missing: fall back to the universal alias once. */
        if (strcmp(xlfd, DRAW_DEFAULT_FONT))
            return draw_font(wm, DRAW_DEFAULT_FONT);
        fprintf(stderr, "austere: cannot load font '%s'\n", xlfd);
        exit(1);
    }

    f = xmalloc(sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", xlfd);
    f->builtin = NULL;
    f->id = id;
    f->ascent = r->font_ascent;
    f->descent = r->font_descent;
    f->min_char = r->min_char_or_byte2;
    f->default_char =
        (unsigned)(r->default_char - r->min_char_or_byte2);
    f->next = wm->fonts;
    f->infos = NULL;
    f->n_infos = 0;
    int n = xcb_query_font_char_infos_length(r);

    if (n > 0) {
        f->infos = xmalloc((size_t)n * sizeof(xcb_charinfo_t));
        memcpy(f->infos, xcb_query_font_char_infos(r),
            (size_t)n * sizeof(xcb_charinfo_t));
        f->n_infos = (unsigned)n;
    }
    free(r);
    wm->fonts = f;
    return f;
}

unsigned
font_height(const font_t *f)
{
    return f ? f->ascent + f->descent : 0;
}

unsigned
font_ascent(const font_t *f)
{
    return f ? f->ascent : 0;
}

unsigned
draw_text_w(wm_t *wm, const font_t *f, const char *s, unsigned len)
{
    unsigned w = 0;

    (void)wm;
    if (!f)
        return 0;
    if (f->builtin) {
        const char *p = s, *e = s + len;

        while (p < e) {
            if (!utf8_cp(&p, e))
                continue;
            w += f->builtin->advance;
        }
        return w;
    }
    for (unsigned i = 0; i < len && s[i]; i++)
        w += char_w(f, (unsigned char)s[i]);
    return w;
}

void
draw_text(wm_t *wm, const draw_t *d, const font_t *f, int x,
    int y_baseline, const char *s, unsigned len, uint32_t fg,
    uint32_t bg)
{
    if (!len || !s || !f)
        return;

    if (f->builtin) {
        /* embedded Agave: glyphs carry 4-bit coverage; every lit pixel
         * blends fg toward the caller's bg so edges antialias against
         * whatever they sit on. Runs of equal coverage share a rect. */
        const builtin_font_t *b = f->builtin;
        const char *p = s, *e = s + len;
        unsigned br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff,
            bb = bg & 0xff;
        int cx = x;

        while (p < e) {
            uint32_t cp = utf8_cp(&p, e);
            size_t off = (size_t)-1;

            if (!cp)
                continue;
            if (cp >= 32u && cp < 32u + b->nglyph)
                off = (size_t)(cp - 32) * b->rowbytes * b->cell_h;
            else
                off = ext_off(b, cp);
            if (off == (size_t)-1) {
                cx += b->advance;
                continue;
            }
            const uint8_t *g = b->bits + off;
            int top = y_baseline - (int)b->ascent;

            for (unsigned row = 0; row < b->cell_h; row++) {
                const uint8_t *r = g + row * b->rowbytes;
                unsigned col = 0;

                while (col < b->advance) {
                    uint8_t cov = (col & 1)
                        ? (uint8_t)(r[col >> 1] & 0xf)
                        : (uint8_t)(r[col >> 1] >> 4);

                    if (!cov) {
                        col++;
                        continue;
                    }
                    uint32_t c =
                        ((((fg >> 16 & 0xff) * cov + br * (15 - cov) +
                            7) / 15) << 16) |
                        ((((fg >> 8 & 0xff) * cov + bgc * (15 - cov) +
                            7) / 15) << 8) |
                        (((fg & 0xff) * cov + bb * (15 - cov) + 7) /
                            15);
                    unsigned run = 1;

                    while (col + run < b->advance &&
                        (((col + run) & 1
                                ? (uint8_t)(r[(col + run) >> 1] & 0xf)
                                : (uint8_t)(r[(col + run) >> 1] >>
                                4)) == cov))
                        run++;
                    xcb_rectangle_t rc = {
                        (int16_t)(cx + (int)col),
                        (int16_t)(top + (int)row), (uint16_t)run, 1
                    };

                    xcb_change_gc(wm->conn, d->gc, XCB_GC_FOREGROUND,
                        (uint32_t[]){ c });
                    xcb_poly_fill_rectangle(wm->conn, d->win, d->gc,
                        1, &rc);
                    col += run;
                }
            }
            cx += b->advance;
        }
        return;
    }

    /* image_text fills glyph cells with the GC background, so the bg
     * must differ from fg or every glyph renders as a solid block. */
    xcb_change_gc(wm->conn, d->gc,
        XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT,
        (uint32_t[]){ fg, bg, f->id });
    xcb_image_text_8(wm->conn, (uint8_t)len, d->win, d->gc,
        (int16_t)x, (int16_t)y_baseline, s);
}

void
draw_setup(wm_t *wm, draw_t *d, xcb_window_t win)
{
    d->win = win;
    d->gc = xcb_generate_id(wm->conn);
    xcb_create_gc(wm->conn, d->gc, win,
        XCB_GC_GRAPHICS_EXPOSURES, (uint32_t[]){ 0 });
}

void
draw_shutdown(wm_t *wm)
{
    font_t *f = wm->fonts;

    while (f) {
        font_t *next = f->next;

        free(f->infos);
        free(f);
        f = next;
    }
    wm->fonts = NULL;
}

void
draw_rect(wm_t *wm, const draw_t *d, int x, int y, unsigned w,
    unsigned h, uint32_t color)
{
    xcb_rectangle_t r = { (int16_t)x, (int16_t)y, (uint16_t)w,
        (uint16_t)h };

    xcb_change_gc(wm->conn, d->gc, XCB_GC_FOREGROUND,
        (uint32_t[]){ color });
    xcb_poly_fill_rectangle(wm->conn, d->win, d->gc, 1, &r);
}

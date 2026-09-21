#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "settings.h"
#include "draw.h"
#include "util.h"

#define GLYPH_CACHE_BITS 10 /* slots per font: 1024 characters */
#define GLYPH_CACHE_SIZE (1u << GLYPH_CACHE_BITS)
#define MAX_FACES 8 /* primary + fallbacks from FcFontSort */

struct font {
    char name[128];
    FT_Face faces[MAX_FACES];
    unsigned nfaces;
    unsigned ascent;
    unsigned descent;
    struct glyph {
        uint32_t cp;
        FT_UInt idx;
        FT_Face face;
        unsigned advance;
        int off_x;
        unsigned off_y; /* rows above the baseline */
        unsigned w, h;
        uint8_t *cov; /* NULL until rasterized */
    } cache[GLYPH_CACHE_SIZE];
    font_t *next;
};

typedef struct glyph glyph_t;

static void gc_fg(wm_t *wm, draw_t *d, uint32_t color);
static glyph_t *glyph_get(const font_t *f, uint32_t cp, bool render);

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
    (*p)++;
    return cp;
}

static FT_Library
ftlib(wm_t *wm)
{
    if (!wm->ftlib) {
        FT_Library lib;

        if (FT_Init_FreeType(&lib)) {
            fprintf(stderr, "austere: freetype init failed\n");
            exit(1);
        }
        wm->ftlib = lib;
    }
    return (FT_Library)wm->ftlib;
}

font_t *
draw_font(wm_t *wm, const char *pattern)
{
    if (!pattern || !*pattern)
        pattern = DRAW_DEFAULT_FONT;

    for (font_t *f = wm->fonts; f; f = f->next)
        if (!strcmp(f->name, pattern))
            return f;

    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    int px = 16;

    if (!pat) {
        fprintf(stderr, "austere: bad font pattern '%s'\n", pattern);
        return draw_font(wm, DRAW_DEFAULT_FONT);
    }
    char got_family[128], req_family[128];
    FcChar8 *gf;
    FcPatternGetString(pat, FC_FAMILY, 0, &gf);
    if (gf)
        snprintf(req_family, sizeof(req_family), "%s", gf);
    else
        req_family[0] = '\0';
    FcDefaultSubstitute(pat);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcPatternGetInteger(pat, FC_PIXEL_SIZE, 0, &px);
    FcResult result;
    FcFontSet *set = FcFontSort(NULL, pat, FcFalse, NULL, &result);
    gf = NULL;
    if (set && set->nfont &&
        FcPatternGetString(set->fonts[0], FC_FAMILY, 0, &gf) ==
            FcResultMatch)
        snprintf(got_family, sizeof(got_family), "%s", gf);
    else
        got_family[0] = '\0';
    FcPatternDestroy(pat);

    font_t *f = xmalloc(sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", pattern);
    f->nfaces = 0;
    f->ascent = 0;
    f->descent = 0;
    memset(f->cache, 0, sizeof(f->cache));
    f->next = wm->fonts;

    FT_Library lib = ftlib(wm);

    if (set) {
        for (int i = 0; i < set->nfont && f->nfaces < MAX_FACES; i++) {
            FcPattern *fp = set->fonts[i];
            FcChar8 *file = NULL;
            int index = 0;
            FT_Face face;

            if (FcPatternGetString(fp, FC_FILE, 0, &file) !=
                    FcResultMatch)
                continue;
            FcPatternGetInteger(fp, FC_INDEX, 0, &index);
            if (FT_New_Face(lib, (const char *)file, index, &face))
                continue;
            FT_Set_Pixel_Sizes(face, 0, px);
            if (f->nfaces == 0 || f->ascent == 0) {
                f->ascent =
                    (unsigned)((face->size->metrics.ascender + 32) / 64);
                f->descent = (unsigned)
                    ((-face->size->metrics.descender + 32) / 64);
                if (f->ascent == 0 && f->descent == 0) {
                    f->ascent = (px + 1) / 2;
                    f->descent = px - f->ascent;
                }
            }
            f->faces[f->nfaces++] = face;
        }
        FcFontSetDestroy(set);
    }

    if (f->nfaces == 0) {
        free(f);
        if (strcmp(pattern, DRAW_DEFAULT_FONT)) {
            fprintf(stderr, "austere: no font for '%s'\n", pattern);
            return draw_font(wm, DRAW_DEFAULT_FONT);
        }
        fprintf(stderr, "austere: cannot load any font\n");
        exit(1);
    }
    if (!f->ascent)
        f->ascent = (px + 1) / 2;
    if (!f->descent)
        f->descent = px - f->ascent;
    if (*req_family && *got_family &&
        strcasecmp(req_family, got_family))
        fprintf(stderr, "austere: font '%s' resolved to '%s', not '%s'\n",
            pattern, got_family, req_family);
    wm->fonts = f;
    return f;
}

font_t *
draw_ui_font(wm_t *wm)
{
    return draw_font(wm, cfg.font && *cfg.font ? cfg.font : NULL);
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

static void
glyph_rasterize(glyph_t *g)
{
    FT_GlyphSlot sl;

    if (FT_Load_Glyph(g->face, g->idx, FT_LOAD_RENDER))
        return;
    sl = g->face->glyph;
    g->off_x = sl->bitmap_left;
    g->off_y = sl->bitmap_top;
    g->w = sl->bitmap.width;
    g->h = sl->bitmap.rows;

    if (g->w && g->h) {
        g->cov = xmalloc((size_t)g->w * g->h);
        for (unsigned r = 0; r < g->h; r++)
            memcpy(g->cov + (size_t)r * g->w,
                sl->bitmap.buffer + (size_t)r * sl->bitmap.pitch,
                g->w);
    }
}

/* Cache lookup for one codepoint. A cache miss resolves which face
 * carries the glyph (the primary face unless it lacks it) and reads its
 * advance; rasterization is deferred until a draw actually needs the
 * pixels so draw_text_w stays cheap. */
static glyph_t *
glyph_get(const font_t *f, uint32_t cp, bool render)
{
    glyph_t *g = (glyph_t *)&f->cache[cp & (GLYPH_CACHE_SIZE - 1)];

    if (g->cp != cp) {
        FT_Face face = NULL;
        FT_UInt idx = 0;

        for (unsigned i = 0; i < f->nfaces; i++) {
            idx = FT_Get_Char_Index(f->faces[i], cp);
            if (idx) {
                face = f->faces[i];
                break;
            }
        }
        if (!face) {
            face = f->faces[0];
            idx = 0; /* .notdef */
        }
        if (g->cov)
            free(g->cov);
        g->cp = cp;
        g->idx = idx;
        g->face = face;
        g->cov = NULL;
        g->w = g->h = 0;
        g->off_x = 0;
        g->off_y = 0;
        g->advance = 0;
        if (!FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT))
            g->advance =
                (unsigned)((face->glyph->advance.x + 32) / 64);
        if (!g->advance)
            g->advance = f->ascent ? f->ascent : 8;
    }
    if (render && !g->cov)
        glyph_rasterize(g);
    return g;
}

unsigned
draw_text_w(wm_t *wm, const font_t *f, const char *s, unsigned len)
{
    const char *p = s, *e = s + len;
    unsigned w = 0;

    (void)wm;
    if (!f)
        return 0;
    while (p < e) {
        uint32_t cp = utf8_cp(&p, e);

        if (!cp)
            continue;
        w += glyph_get(f, cp, false)->advance;
    }
    return w;
}

static uint32_t
blend(uint32_t fg, uint32_t bg, uint32_t cov)
{
    uint32_t r = ((fg >> 16 & 0xff) * cov + (bg >> 16 & 0xff) *
        (255 - cov) + 127) / 255;
    uint32_t g = ((fg >> 8 & 0xff) * cov + (bg >> 8 & 0xff) *
        (255 - cov) + 127) / 255;
    uint32_t b = ((fg & 0xff) * cov + (bg & 0xff) * (255 - cov) + 127) /
        255;

    return (fg & 0xff000000u) | r << 16 | g << 8 | b;
}

void
draw_text(wm_t *wm, draw_t *d, const font_t *f, int x,
    int y_baseline, const char *s, unsigned len, uint32_t fg,
    uint32_t bg)
{
    const char *p = s, *e = s + len;
    unsigned asc, desc, h, w = 0;
    uint32_t *buf;

    if (!len || !s || !f)
        return;
    asc = f->ascent;
    desc = f->descent;
    h = asc + desc;
    if (!h)
        return;

    while (p < e) {
        uint32_t cp = utf8_cp(&p, e);

        if (!cp)
            continue;
        w += glyph_get(f, cp, false)->advance;
    }
    if (!w)
        return;

    if (wm->textbuf_cap < (size_t)w * h) {
        size_t need = (size_t)w * h;

        wm->textbuf = realloc(wm->textbuf, need * sizeof(uint32_t));
        if (!wm->textbuf) {
            wm->textbuf_cap = 0;
            return;
        }
        wm->textbuf_cap = (unsigned)need;
    }
    buf = wm->textbuf;
    for (size_t i = 0; i < (size_t)w * h; i++)
        buf[i] = bg;

    p = s;
    for (int pen = 0; p < e && (unsigned)pen < w;) {
        uint32_t cp = utf8_cp(&p, e);
        glyph_t *g;

        if (!cp)
            continue;
        g = glyph_get(f, cp, true);
        if (g->w && g->h) {
            for (unsigned yy = 0; yy < g->h; yy++) {
                int top = (int)asc - (int)g->off_y + (int)yy;
                const uint8_t *cov = g->cov + (size_t)yy * g->w;

                if (top < 0 || top >= (int)h)
                    continue;
                for (unsigned xx = 0; xx < g->w; xx++) {
                    int left = pen + g->off_x + (int)xx;
                    uint8_t c = cov[xx];
                    uint32_t *px;

                    if (left < 0 || left >= (int)w)
                        continue;
                    px = buf + (size_t)top * w + (unsigned)left;
                    if (c == 255)
                        *px = fg;
                    else if (c)
                        *px = blend(fg, bg, c);
                }
            }
        }
        pen += (int)g->advance;
    }

    draw_put_image24(wm, d->win, d->gc, d->depth, (int16_t)x,
        (int16_t)(y_baseline - (int)asc), w, h, buf);
}

void
draw_setup(wm_t *wm, draw_t *d, xcb_window_t win)
{
    xcb_get_geometry_cookie_t ck;
    xcb_get_geometry_reply_t *r;

    d->win = win;
    d->gc = xcb_generate_id(wm->conn);
    d->gc_fg_known = false;
    d->depth = (uint8_t)wm->scr->root_depth;
    xcb_create_gc(wm->conn, d->gc, win,
        XCB_GC_GRAPHICS_EXPOSURES, (uint32_t[]){ 0 });
    ck = xcb_get_geometry(wm->conn, win);
    r = xcb_get_geometry_reply(wm->conn, ck, NULL);
    if (r) {
        d->depth = (uint8_t)r->depth;
        free(r);
    }
}

/* Program the GC foreground once per distinct color: poly_fill only
 * reads FOREGROUND, so a repeat change_gc for the same pixel is pure
 * wire traffic. draw_text keeps this cache current for every path that
 * sets fg. */
static void
gc_fg(wm_t *wm, draw_t *d, uint32_t color)
{
    if (d->gc_fg_known && d->gc_fg == color)
        return;
    xcb_change_gc(wm->conn, d->gc, XCB_GC_FOREGROUND,
        (uint32_t[]){ color });
    d->gc_fg = color;
    d->gc_fg_known = true;
}

void
draw_shutdown(wm_t *wm)
{
    font_t *f = wm->fonts;

    while (f) {
        font_t *next = f->next;

        for (unsigned i = 0; i < GLYPH_CACHE_SIZE; i++)
            free(f->cache[i].cov);
        for (unsigned i = 0; i < f->nfaces; i++)
            FT_Done_Face(f->faces[i]);
        free(f);
        f = next;
    }
    wm->fonts = NULL;
    if (wm->ftlib) {
        FT_Done_FreeType((FT_Library)wm->ftlib);
        wm->ftlib = NULL;
    }
    FcFini();
    free(wm->textbuf);
    wm->textbuf = NULL;
    wm->textbuf_cap = 0;
}

void
draw_rect(wm_t *wm, draw_t *d, int x, int y, unsigned w,
    unsigned h, uint32_t color)
{
    xcb_rectangle_t r = { (int16_t)x, (int16_t)y, (uint16_t)w,
        (uint16_t)h };

    gc_fg(wm, d, color);
    xcb_poly_fill_rectangle(wm->conn, d->win, d->gc, 1, &r);
}

static const xcb_format_t *
fmt_for_depth(xcb_connection_t *conn, unsigned depth)
{
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_format_iterator_t it = xcb_setup_pixmap_formats_iterator(setup);

    while (it.rem) {
        if (it.data->depth == depth)
            return it.data;
        xcb_format_next(&it);
    }
    return NULL;
}

void
draw_put_image24(wm_t *wm, xcb_window_t win, xcb_gcontext_t gc,
    uint8_t depth, int16_t x, int16_t y, unsigned w, unsigned h,
    const uint32_t *argb)
{
    xcb_image_order_t order;
    size_t psize, pad, row;
    uint8_t *bytes;
    const xcb_format_t *f = fmt_for_depth(wm->conn, depth);

    if (f == NULL)
        return;
    /* Pack per the server's advertised Z-format for this depth:
     * bits-per-pixel bytes each pixel (24-bit depth commonly maps to
     * 4 bytes on modern servers), scanlines a multiple of scanline-pad. */
    order = xcb_get_setup(wm->conn)->image_byte_order;
    psize = f->bits_per_pixel / 8;
    pad = f->scanline_pad / 8;
    row = (w * psize + pad - 1) / pad * pad;
    bytes = xmalloc(row * h);
    memset(bytes, 0, row * h);

    for (unsigned yy = 0; yy < h; yy++) {
        uint8_t *dst = bytes + yy * row;

        for (unsigned xx = 0; xx < w; xx++) {
            uint32_t v = argb[yy * w + xx];
            const uint8_t *s = (const uint8_t *)&v;

            for (size_t k = 0; k < psize; k++)
                dst[xx * psize + k] = order == XCB_IMAGE_ORDER_LSB_FIRST
                    ? s[k]
                    : s[psize - 1 - k];
        }
    }
    xcb_put_image(wm->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc,
        (uint16_t)w, (uint16_t)h, x, y, 0, depth, row * h, bytes);
    free(bytes);
}
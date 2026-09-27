#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <xcb/xcb_keysyms.h>

#include "bar.h"
#include "draw.h"
#include "menu.h"
#include "monitor.h"
#include "settings.h"
#include "tray.h"
#include "tray_menu_ui.h"
#include "util.h"

/* The tray item's own flat menu as a native popup (SPEC §7.6.1).
 *
 * Presentation only. The backend owns every label, state bit, image and
 * the title; this file draws what tray_menu_view() publishes and hands an
 * activated row's snapshot index straight back to tray_menu_click(). It
 * allocates nothing per menu, frees nothing it does not own, and never
 * touches the bus: the popup is a pure function of the published state,
 * so it may be destroyed and rebuilt at any moment without a client
 * noticing.
 *
 * Shape and placement, all of it decided in tm_layout() so the window, the
 * drawing and the hit-test can never disagree:
 *   - one row per published row, all of the same height (the settings
 *     panel's: font height + 6, never under 16), so a row's band is its
 *     index times that height and nothing else,
 *   - rows hang below the press, flipped to the workarea's bottom edge
 *     when they do not fit (a bottom bar's tray icon is below the
 *     workarea), and the whole window is clamped inside it,
 *   - the check column exists only when some row is a toggle or a radio
 *     item, and the icon column only when some row brings an icon, so a
 *     plain menu is not indented for nothing,
 *   - a menu taller than the workarea shows its leading rows and a dim
 *     extent tick; nothing scrolls in this phase.
 *
 * Austere's own surfaces are the reference: bar_bg ground, bar_fg text,
 * focus_color for the frame that makes a floating window legible (the
 * toast border), BAR_DIM for anything secondary or inert, and the
 * panel's 6 px label inset with its row bands. No new palette, no new
 * font, no new padding idea. */

#define TM_PAD 6           /* the panel's label inset */
#define TM_MARK 8          /* check column: a 7 px mark plus its air */
#define TM_MARK_GAP 5
#define TM_ICON_GAP 6
#define TM_ICON_MAX 24     /* a row icon is a mark, not a banner */
#define TM_ROW_H_MIN 16    /* menu.c's floor */
#define TM_ROW_PAD 6       /* menu.c's row padding */
#define TM_RULE 1          /* header rule, extent tick */
#define TM_MIN_W 120
#define TM_MAX_W 320       /* menu.c's app-menu width */
#define TM_TICK_W 2

typedef struct {
    int x, y;              /* window position, root coordinates */
    unsigned w, h;
    unsigned row_h;
    unsigned head_h;       /* the title row and its rule, 0 when untitled */
    unsigned nvis;         /* rows drawn: never more than nrows */
    bool checks;           /* the check column is reserved */
    unsigned label_x;      /* where the label column starts */
    unsigned max_icon;     /* widest row icon, 0 when there is none */
} layout_t;

static xcb_window_t win = XCB_NONE;
static draw_t draw;
static unsigned cur_w, cur_h;   /* last applied geometry */
static int cur_x, cur_y;
static unsigned epoch;          /* the epoch currently on screen */
static bool epoch_valid;

/* ---- metrics ---------------------------------------------------------- */

static unsigned
row_h(wm_t *wm)
{
    unsigned h = font_height(draw_ui_font(wm)) + TM_ROW_PAD;

    return h > TM_ROW_H_MIN ? h : TM_ROW_H_MIN;
}

/* The monitor holding a root point, else the focused one. The anchor is
 * a press on a bar, so it is inside some monitor even when it sits in
 * the strip that mon_workarea() has already removed. */
static monitor_t *
tm_monitor(wm_t *wm, int x, int y)
{
    for (monitor_t *m = wm->mons; m; m = m->next)
        if (x >= m->geom.x && x < m->geom.x + (int)m->geom.w &&
            y >= m->geom.y && y < m->geom.y + (int)m->geom.h)
            return m;
    return focused_mon(wm);
}

/* A row icon, fit by height with its aspect kept, and refused when the
 * result would not stay a mark. Layout and paint both ask here, so the
 * column the labels are indented by is the column the pixels land in. */
static bool
icon_fit(const image_t *im, unsigned *w, unsigned *h)
{
    if (!im || !im->argb || !im->w || !im->h)
        return false;
    unsigned ih = im->h > TM_ICON_MAX ? TM_ICON_MAX : im->h;
    unsigned iw = (im->w * ih + im->h / 2) / im->h;

    if (!iw || iw > TM_ICON_MAX)
        return false;
    *w = iw;
    *h = ih;
    return true;
}

static void
tm_layout(wm_t *wm, const tray_menu_view_t *v, layout_t *L)
{
    font_t *f = draw_ui_font(wm);
    Rect a = mon_workarea(tm_monitor(wm, v->anchor_x, v->anchor_y));
    unsigned rh = row_h(wm);
    unsigned label = 0, icon = 0, iw, ih;
    bool checks = false;

    for (unsigned i = 0; i < v->nrows; i++) {
        const tray_menu_row_t *r = &v->rows[i];

        if (r->type == TRAY_MENU_ROW_TOGGLE ||
            r->type == TRAY_MENU_ROW_RADIO)
            checks = true;
        if (icon_fit(r->img, &iw, &ih) && iw > icon)
            icon = iw;
        if (r->label && *r->label) {
            unsigned w = draw_text_w(wm, f, r->label,
                (unsigned)strlen(r->label));

            if (w > label)
                label = w;
        }
    }
    L->row_h = rh;
    L->head_h = v->title && *v->title ? rh + TM_RULE : 0;
    L->checks = checks;
    L->max_icon = icon;
    L->label_x = TM_PAD + (checks ? TM_MARK + TM_MARK_GAP : 0) +
        (icon ? icon + TM_ICON_GAP : 0);

    /* the workarea is the whole budget: what does not fit is not drawn,
     * and therefore not clickable either */
    unsigned room = a.h > L->head_h ? a.h - L->head_h : 0;
    unsigned fit = room / rh;

    L->nvis = v->nrows < fit ? v->nrows : fit;
    L->w = L->label_x + label + TM_PAD;
    if (L->w < TM_MIN_W)
        L->w = TM_MIN_W;
    if (L->w > TM_MAX_W)
        L->w = TM_MAX_W;
    if (L->w > a.w)
        L->w = a.w;
    L->h = L->head_h + L->nvis * rh;

    int x = v->anchor_x;

    if (x + (int)L->w > a.x + (int)a.w)
        x = v->anchor_x - (int)L->w;     /* flip left of the anchor */
    if (x < a.x)
        x = a.x;
    if (x + (int)L->w > a.x + (int)a.w)
        x = a.x + (int)a.w - (int)L->w;
    L->x = x;
    int y;

    if (v->anchor_y + (int)L->h <= a.y + (int)a.h)
        y = v->anchor_y;                 /* hangs below the press */
    else
        y = a.y + (int)a.h - (int)L->h;  /* flipped: flush with the bar */
    if (y < a.y)
        y = a.y;
    if (y + (int)L->h > a.y + (int)a.h)
        y = a.y + (int)a.h - (int)L->h;
    L->y = y;
}

/* Row i owns exactly [head_h + i*row_h, head_h + (i+1)*row_h) in window
 * coordinates. One line of arithmetic, used by the paint loop and the
 * hit-test, is the whole hit-test. */
static bool
tm_row_at(const layout_t *L, int wy, unsigned *row)
{
    if (wy < (int)L->head_h)
        return false;
    unsigned i = (unsigned)(wy - (int)L->head_h) / L->row_h;

    if (i >= L->nvis)
        return false;
    *row = i;
    return true;
}

/* ---- painting --------------------------------------------------------- */

/* ---- compositing ------------------------------------------------------
 *
 * image_t.argb is STRAIGHT (unassociated) ARGB32: A in bits 24..31, then
 * R, G, B, and a pixel with A == 0 carries no colour at all. Both sources
 * that reach this surface agree on that, which is worth measuring rather
 * than assuming:
 *   - the Imlib2 theme decode hands over straight alpha. hicolor's
 *     caffeine-cup-empty stores RGB 255,255,255 under every fully
 *     transparent pixel, which is what a PNG with an RGB channel kept
 *     beside an empty alpha looks like,
 *   - an SNI IconPixmap is network-order ARGB32 on the wire, and the
 *     backend copies those bytes through verbatim.
 * Reading either as premultiplied paints every transparent pixel with the
 * source's own colour - white, for a theme icon - which is the white box
 * that used to sit behind every tray glyph.
 *
 * So there is one formula, and an exact-size blit is its 1x1 degenerate
 * rather than a second path that can drift from the first. For the box of
 * source pixels that map to one destination pixel, average in
 * PREMULTIPLIED space - the mean of r*a, g*a and b*a, and the mean of a -
 * so a transparent pixel contributes no colour to its neighbours, and
 * composite that over the bar ground:
 *
 *   out = (sum(r*a)/n + ground * (255 - sum(a)/n) + 127) / 255, clamped
 *
 * A == 0 lands exactly on the ground, A == 255 exactly on the source, and
 * a half-transparent pixel half way between the two. `tint` replaces the
 * source's own colour (0 keeps it), which is how an inert row keeps an
 * icon's silhouette but loses its colour in the tray menu; the bar never
 * dims, and a dim tone is never black.
 *
 * The sums are bounded by the box, i.e. the source pixel count behind one
 * destination pixel: a tray source is at most 512px and the box a few
 * hundred pixels at most, so a 32-bit accumulator cannot wrap. */
static uint32_t
composite_pixel(const image_t *im, unsigned dx, unsigned dw, unsigned dy,
    unsigned dh, uint32_t ground, uint32_t tint)
{
    unsigned x0 = dx * im->w / dw, y0 = dy * im->h / dh;
    unsigned x1 = ((dx + 1) * im->w + dw - 1) / dw;
    unsigned y1 = ((dy + 1) * im->h + dh - 1) / dh;
    uint32_t aa = 0, ar = 0, ag = 0, ab = 0, n = 0;
    uint32_t gr = (ground >> 16) & 0xff, gg = (ground >> 8) & 0xff,
        gb = ground & 0xff, av, pr, pg, pb, orr, ogg, obb;

    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 <= y0)
        y1 = y0 + 1;
    if (x1 > im->w)
        x1 = im->w;
    if (y1 > im->h)
        y1 = im->h;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++) {
            uint32_t p = im->argb[(size_t)y * im->w + x];
            uint32_t a = p >> 24;

            /* premultiplied on the way in: a transparent pixel carries no
             * colour into the average */
            aa += a;
            ar += a * ((p >> 16) & 0xff);
            ag += a * ((p >> 8) & 0xff);
            ab += a * (p & 0xff);
            n++;
        }
    if (!n)
        return 0xff000000u | ground;      /* no source pixel: the ground */
    av = aa / n;
    if (tint) {
        pr = ((tint >> 16) & 0xff) * av;
        pg = ((tint >> 8) & 0xff) * av;
        pb = (tint & 0xff) * av;
    } else {
        pr = ar / n;
        pg = ag / n;
        pb = ab / n;
    }
    uint32_t through = 255 - av;

    orr = (pr + gr * through + 127) / 255;
    ogg = (pg + gg * through + 127) / 255;
    obb = (pb + gb * through + 127) / 255;
    if (orr > 255)
        orr = 255;
    if (ogg > 255)
        ogg = 255;
    if (obb > 255)
        obb = 255;
    return 0xff000000u | orr << 16 | ogg << 8 | obb;
}

/* One row icon, composited over the menu ground. An inert row keeps its
 * icon's silhouette but loses its colour: `tint` is the row's dim tone,
 * so a disabled row reads as one thing instead of a dim label beside a
 * live-looking icon. */
static void
tm_icon(wm_t *wm, const image_t *im, int x, int y, unsigned w, unsigned h,
    bool dim, uint32_t tint)
{
    uint32_t buf[TM_ICON_MAX * TM_ICON_MAX];

    for (unsigned dy = 0; dy < h; dy++)
        for (unsigned dx = 0; dx < w; dx++)
            buf[(size_t)dy * w + dx] = composite_pixel(im, dx, w, dy, h,
                cfg.bar_bg, dim ? tint : 0);
    draw_put_image24(wm, win, draw.gc, draw.depth, (int16_t)x, (int16_t)y, w,
        h, buf);
}

/* State marks in primitives, so no font has to carry a glyph: a checked
 * toggle is a filled box, a checked radio item a ring around a dot.
 * Unchecked draws nothing, which is what a menu means by it. */
static void
tm_mark(wm_t *wm, int x, int y, const tray_menu_row_t *r, uint32_t col)
{
    if (!r->checked)
        return;
    if (r->type == TRAY_MENU_ROW_TOGGLE) {
        draw_rect(wm, &draw, x + 1, y + 1, 5, 5, col);
        return;
    }
    if (r->type != TRAY_MENU_ROW_RADIO)
        return;
    draw_rect(wm, &draw, x, y, 7, TM_RULE, col);
    draw_rect(wm, &draw, x, y + 6, 7, TM_RULE, col);
    draw_rect(wm, &draw, x, y, TM_RULE, 7, col);
    draw_rect(wm, &draw, x + 6, y, TM_RULE, 7, col);
    draw_rect(wm, &draw, x + 2, y + 2, 3, 3, col);
}

static void
tm_paint(wm_t *wm, const tray_menu_view_t *v, const layout_t *L)
{
    font_t *f = draw_ui_font(wm);
    uint32_t bg = cfg.bar_bg, fg = cfg.bar_fg, ac = cfg.focus_color;
    unsigned rh = L->row_h, lh = font_height(f), asc = font_ascent(f);
    unsigned iw, ih;

    draw_rect(wm, &draw, 0, 0, L->w, L->h, bg);
    /* the toast's accent hairline: a small dark window needs an edge to
     * read as a surface on an arbitrary wallpaper */
    draw_rect(wm, &draw, 0, 0, L->w, TM_RULE, ac);
    draw_rect(wm, &draw, 0, (int)L->h - TM_RULE, L->w, TM_RULE, ac);
    draw_rect(wm, &draw, 0, 0, TM_RULE, L->h, ac);
    draw_rect(wm, &draw, (int)L->w - TM_RULE, 0, TM_RULE, L->h, ac);
    if (L->head_h) {
        int base = ((int)rh - (int)lh) / 2 + (int)asc;

        draw_text(wm, &draw, f, TM_PAD, base, v->title,
            (unsigned)strlen(v->title), fg, bg);
        draw_rect(wm, &draw, 0, (int)L->head_h - TM_RULE, L->w, TM_RULE,
            BAR_DIM);
    }
    for (unsigned i = 0; i < L->nvis; i++) {
        const tray_menu_row_t *r = &v->rows[i];
        int y0 = (int)L->head_h + (int)i * (int)rh;
        int base = y0 + ((int)rh - (int)lh) / 2 + (int)asc;
        uint32_t col = r->enabled ? fg : BAR_DIM;

        if (r->type == TRAY_MENU_ROW_SEPARATOR) {
            draw_rect(wm, &draw, TM_PAD, y0 + (int)rh / 2,
                L->w - 2 * TM_PAD, TM_RULE, BAR_DIM);
            continue;
        }
        if (L->checks)
            tm_mark(wm, TM_PAD, y0 + ((int)rh - 7) / 2, r, col);
        if (L->max_icon && icon_fit(r->img, &iw, &ih)) {
            int ix = TM_PAD + (L->checks ? TM_MARK + TM_MARK_GAP : 0);

            tm_icon(wm, r->img, ix, y0 + ((int)rh - (int)ih) / 2, iw, ih,
                !r->enabled, col);
        }
        if (r->label && *r->label)
            draw_text(wm, &draw, f, (int)L->label_x, base, r->label,
                (unsigned)strlen(r->label), col, bg);
    }
    /* Rows that did not fit: the settings panel's dim scrollbar, without
     * the scrolling - it says how much of the menu is on screen. */
    if (v->nrows > L->nvis && L->nvis) {
        unsigned list = L->nvis * rh;
        unsigned tick = list * L->nvis / v->nrows;

        if (tick < TM_TICK_W)
            tick = TM_TICK_W;
        if (tick > list)
            tick = list;
        draw_rect(wm, &draw, (int)L->w - TM_PAD, (int)L->head_h, TM_TICK_W,
            tick, BAR_DIM);
    }
}

/* ---- window lifecycle -------------------------------------------------- */

static void
tm_destroy(wm_t *wm)
{
    if (win == XCB_NONE)
        return;
    xcb_free_gc(wm->conn, draw.gc);
    xcb_destroy_window(wm->conn, win);
    memset(&draw, 0, sizeof(draw));
    win = XCB_NONE;
    epoch_valid = false;
}

/* A dismissal: the backend drops the menu, and the window goes with it.
 * Row activation is not a dismissal - the backend decides whether a
 * toggled menu stays up, and the next sync follows it. */
static void
tm_dismiss(wm_t *wm)
{
    tray_menu_close();
    tm_destroy(wm);
}

static void
tm_create(wm_t *wm, const layout_t *L)
{
    win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, win, wm->scr->root,
        (int16_t)L->x, (int16_t)L->y, (uint16_t)L->w, (uint16_t)L->h, 1,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
        (uint32_t[]){ cfg.bar_bg, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_KEY_PRESS });
    draw_setup(wm, &draw, win);
    cur_w = L->w;
    cur_h = L->h;
    cur_x = L->x;
    cur_y = L->y;
    xcb_map_window(wm->conn, win);
    epoch_valid = false;
}

void
tray_menu_ui_sync(wm_t *wm)
{
    tray_menu_view_t v = tray_menu_view();
    layout_t L;
    bool repaint = false;

    if (v.state != TRAY_MENU_READY || !v.rows || !v.nrows) {
        /* LOADING included: the window of a previous menu must not
         * outlive the state that asked for it */
        tm_destroy(wm);
        return;
    }
    tm_layout(wm, &v, &L);
    if (!L.nvis) {       /* a workarea too short for one row */
        tm_destroy(wm);
        return;
    }
    if (win == XCB_NONE) {
        /* the settings panel grabs the keyboard and paints over the bar:
         * the tray menu wins the screen it was asked for */
        if (menu_active())
            menu_close(wm);
        tm_create(wm, &L);
        repaint = true;
    } else if (L.w != cur_w || L.h != cur_h || L.x != cur_x || L.y != cur_y) {
        uint32_t vals[] = { (uint32_t)L.x, (uint32_t)L.y, L.w, L.h };

        xcb_configure_window(wm->conn, win,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
        cur_w = L.w;
        cur_h = L.h;
        cur_x = L.x;
        cur_y = L.y;
        /* the server clears a reconfigured window, so this is not optional */
        repaint = true;
    }
    if (!epoch_valid || epoch != v.epoch)
        repaint = true;
    if (!repaint)
        return;
    tm_paint(wm, &v, &L);
    epoch = v.epoch;
    epoch_valid = true;
    /* the loop's next flush can be a poll timeout away, and a
     * half-flushed batch leaves a window painted in the wrong colours */
    xcb_flush(wm->conn);
    raise_window(wm, win);
}

bool
tray_menu_ui_button(wm_t *wm, xcb_button_press_event_t *ev)
{
    tray_menu_view_t v;
    layout_t L;
    unsigned row;

    if (win == XCB_NONE)
        return false;
    if (ev->event != win) {
        /* A press on a bar window closes the menu but still belongs to the
         * module under the pointer: clicking another tray icon switches
         * menus instead of only dismissing. Anywhere else - a client, a
         * panel - the press is swallowed, as a click-away menu must. */
        for (monitor_t *m = wm->mons; m; m = m->next)
            if (m->bar && m->bar->win == ev->event) {
                tm_dismiss(wm);
                return false;
            }
        tm_dismiss(wm);
        return true;
    }
    v = tray_menu_view();
    if (v.state != TRAY_MENU_READY || !v.rows) {
        tm_destroy(wm);
        return true;
    }
    tm_layout(wm, &v, &L);
    if (!tm_row_at(&L, ev->event_y, &row))
        return true;                     /* the title band, or below it */
    if (v.rows[row].type == TRAY_MENU_ROW_SEPARATOR || !v.rows[row].enabled)
        return true;                     /* drawn, drawn-inert */
    if (ev->detail != XCB_BUTTON_INDEX_1)
        return true;                     /* only button 1 activates */
    tray_menu_click(row);
    return true;
}

bool
tray_menu_ui_key(wm_t *wm, xcb_key_press_event_t *ev)
{
    if (win == XCB_NONE)
        return false;
    if (xcb_key_symbols_get_keysym(wm->keysyms, ev->detail, 0) != 0xff1b)
        return false;    /* no focus, no grab: the client keeps its keys */
    tm_dismiss(wm);
    return true;
}

bool
tray_menu_ui_expose(wm_t *wm, xcb_window_t w)
{
    tray_menu_view_t v;
    layout_t L;

    if (win == XCB_NONE || w != win)
        return false;
    v = tray_menu_view();
    if (v.state != TRAY_MENU_READY || !v.rows) {
        tm_destroy(wm);
        return true;
    }
    tm_layout(wm, &v, &L);
    tm_paint(wm, &v, &L);
    return true;
}

bool
tray_menu_ui_owns_window(xcb_window_t w)
{
    return win != XCB_NONE && w == win;
}

bool
tray_menu_ui_active(void)
{
    return win != XCB_NONE;
}

void
tray_menu_ui_shutdown(wm_t *wm)
{
    tm_destroy(wm);
}

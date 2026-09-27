#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include "../bar.h"
#include "../barmod.h"
#include "../draw.h"
#include "../settings.h"
#include "../tray.h"

/* StatusNotifierItem tray as a bar module (SPEC §7.6, phase 2).
 *
 * Presentation only. The backend owns the registry, the item order, the
 * pixels and every wire call; this file reads tray_view() and dispatches
 * tray_click() / tray_scroll(). No D-Bus here, not even indirectly: the
 * event loop owns the bus and turns the backend's render flag into at
 * most one repaint, so a render here can never recurse.
 *
 * Layout, all of it decided in tray_layout() so measure, draw and
 * hit-test cannot disagree:
 *   - icon box is the bar's inner height (bar height - 2*BAR_HEIGHT_PAD,
 *     which is exactly the ui font's height; no icon-size key in this
 *     phase),
 *   - images are fit to that height with their aspect ratio kept, so a
 *     non-square icon is never squashed,
 *   - icons are vertically centered, separated by the existing BAR_PAD,
 *     and carry no separator of their own,
 *   - items keep the backend's order; an empty view measures zero and
 *     the bar drops the module entirely,
 *   - a bar too narrow for the whole strip keeps the leading run and
 *     drops the rest, rather than overlapping or clipping an icon.
 *
 * A cell index is the snapshot index, so the leading run of cells lines
 * up with what tray_click() resolves its action against. Passive items
 * and attention icons are already the backend's decision, so nothing
 * here re-reads Status. */

typedef struct {
    unsigned off;          /* x from the module origin */
    unsigned w;            /* drawn width, aspect kept */
    const image_t *img;    /* NULL: the placeholder is drawn instead */
} cell_t;

/* Per-bar scratch: one composited icon, reused along the strip.
 *
 * The pixels are a flexible array member, so the header and the buffer are
 * a single allocation and module_kill()'s free(m->data) is the entire
 * teardown. A separate malloc for the pixels would leak one buffer per bar
 * rebuild, since nothing else in the module owns it. */
typedef struct {
    size_t cap;            /* pixels buf[] holds */
    uint32_t buf[];
} tray_scratch_t;

/* The icon box: the bar's inner height. */
static unsigned
tray_box(wm_t *wm)
{
    return font_height(draw_ui_font(wm));
}

/* Icons are centered in the bar. The click handler reads the same line,
 * so the root point it reports is the pressed icon's own center. */
static int
tray_icon_y(monitor_t *mon, unsigned box)
{
    return ((int)mon->bar->height - (int)box) / 2;
}

/* The one geometry pass. Fills cells[] with the visible run and returns
 * its length; the module's width is the last cell's right edge, or zero
 * when nothing is visible.
 *
 * The budget is the bar's inner width - the most this module could ever
 * legally occupy. A module has no way to see what its neighbours claim
 * (the bar has no cross-module overflow pass), so this is a bound on
 * the tray's own strip, not a share of the bar. */
static unsigned
tray_layout(wm_t *wm, monitor_t *mon, cell_t *cells)
{
    tray_view_t v = tray_view();
    unsigned box = tray_box(wm);
    unsigned budget = mon->geom.w > 2 * cfg.bar_gap
        ? mon->geom.w - 2 * cfg.bar_gap : 0;
    unsigned acc = 0, n = 0;

    if (!box)
        return 0;
    for (unsigned i = 0; i < v.nitems && n < TRAY_MAX_ITEMS; i++) {
        const tray_item_t *it = &v.items[i];
        const image_t *im = it->img;
        bool has = im && im->argb && im->w && im->h;
        unsigned w = box, off, right;

        /* fit to the box by height; an item with no usable image keeps a
         * square cell so the strip's rhythm and the hit-test hold */
        if (has)
            w = (im->w * box + im->h / 2) / im->h;
        if (!w)
            w = 1;
        /* BAR_PAD leads every cell but the first, so consecutive icons
         * sit one pad apart and the bar's own pad keeps the group clear
         * of its neighbours */
        off = n ? acc + BAR_PAD : 0;
        right = off + w;
        if (right > budget)
            break;          /* trailing items are dropped, not overlapped */
        cells[n].off = off;
        cells[n].w = w;
        cells[n].img = has ? im : NULL;
        acc = right;
        n++;
    }
    return n;
}

static unsigned
tray_width(const cell_t *cells, unsigned n)
{
    return n ? cells[n - 1].off + cells[n - 1].w : 0;
}

/* Premultiplied source over an opaque bar pixel, clamped: the SNI
 * IconPixmap path delivers straight alpha, so the invariant is not ours
 * to rely on. */
static unsigned
over(unsigned src, unsigned bg, unsigned a)
{
    src += (bg * (255 - a) + 127) / 255;
    return src > 255 ? 255 : src;
}

/* Area-average one destination pixel. Nearest neighbour turns a 32px
 * IconPixmap into a shimmer at bar height and there is no resampler to
 * borrow; averaging premultiplied ARGB keeps thin strokes and stops a
 * transparent border bleeding colour into the icon. */
static uint32_t
resample(const image_t *im, unsigned dx, unsigned dw, unsigned dy,
    unsigned dh)
{
    unsigned x0 = dx * im->w / dw, y0 = dy * im->h / dh;
    unsigned x1 = ((dx + 1) * im->w + dw - 1) / dw;
    unsigned y1 = ((dy + 1) * im->h + dh - 1) / dh;
    unsigned aa = 0, ar = 0, ag = 0, ab = 0, n = 0;

    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 <= y0)
        y1 = y0 + 1;
    if (x1 > im->w)
        x1 = im->w;
    if (y1 > im->h)
        y1 = im->h;
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) {
            uint32_t p = im->argb[(size_t)y * im->w + x];

            aa += p >> 24;
            ar += (p >> 16) & 0xff;
            ag += (p >> 8) & 0xff;
            ab += p & 0xff;
            n++;
        }
    if (!n || !aa)
        return 0;
    return (aa / n) << 24 | (ar / n) << 16 | (ag / n) << 8 | ab / n;
}

/* Composite one icon onto the bar background and blit it. draw_put_image24
 * drops the alpha byte, so a straight blit would print every transparent
 * pixel as a black block on the bar; one small buffer buys back the
 * icons' own silhouette. */
static void
tray_blit(wm_t *wm, monitor_t *mon, module_t *m, const cell_t *c, int x,
    int y, unsigned box)
{
    const image_t *im = c->img;
    tray_scratch_t *st = m->data;
    uint32_t bg = cfg.bar_bg, br = (bg >> 16) & 0xff, bgn = (bg >> 8) & 0xff,
        bb = bg & 0xff;
    bool exact = im->w == c->w && im->h == box;
    size_t need = (size_t)c->w * box;

    if (!st) {
        /* plain malloc, not xmalloc: the block grows with realloc below,
         * and that path already leaves the bar alone on failure */
        st = malloc(sizeof(*st) + need * sizeof(st->buf[0]));
        if (!st)
            return;     /* no room for this icon: leave the bar alone */
        st->cap = need;
        m->data = st;
    } else if (st->cap < need) {
        tray_scratch_t *grown = realloc(st,
            sizeof(*st) + need * sizeof(st->buf[0]));

        if (!grown)
            return;     /* no room for this icon: leave the bar alone */
        grown->cap = need;
        m->data = st = grown;
    }
    for (unsigned dy = 0; dy < box; dy++)
        for (unsigned dx = 0; dx < c->w; dx++) {
            uint32_t s = exact ? im->argb[(size_t)dy * im->w + dx]
                : resample(im, dx, c->w, dy, box);

            st->buf[(size_t)dy * c->w + dx] = 0xff000000u |
                over(s >> 16 & 0xff, br, s >> 24) << 16 |
                over(s >> 8 & 0xff, bgn, s >> 24) << 8 |
                over(s & 0xff, bb, s >> 24);
        }
    draw_put_image24(wm, mon->bar->win, mon->bar->draw.gc,
        mon->bar->draw.depth, (int16_t)x, (int16_t)y, c->w, box, st->buf);
}

/* No icon, or none that decoded: a dim frame around a dim pip, in the
 * bar's own dim tone. The cell keeps its full width, so the item stays
 * clickable and the strip keeps its rhythm - a blank gap would read as
 * a broken tray and would swallow the click. */
static void
tray_placeholder(wm_t *wm, monitor_t *mon, int x, int y, unsigned w,
    unsigned h)
{
    unsigned pip = h / 4;

    if (h < 8 || w < 8)
        return;
    if (pip < 2)
        pip = 2;
    draw_rect(wm, &mon->bar->draw, x, y, w, 1, BAR_DIM);
    draw_rect(wm, &mon->bar->draw, x, y + (int)h - 1, w, 1, BAR_DIM);
    draw_rect(wm, &mon->bar->draw, x, y, 1, h, BAR_DIM);
    draw_rect(wm, &mon->bar->draw, x + (int)w - 1, y, 1, h, BAR_DIM);
    draw_rect(wm, &mon->bar->draw, x + ((int)w - (int)pip) / 2,
        y + ((int)h - (int)pip) / 2, pip, pip, BAR_DIM);
}

static unsigned
traymod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    cell_t cells[TRAY_MAX_ITEMS];
    unsigned n = tray_layout(wm, mon, cells);

    if (!draw || !n)
        return tray_width(cells, n);
    unsigned box = tray_box(wm);
    int y = tray_icon_y(mon, box);

    for (unsigned i = 0; i < n; i++) {
        int cx = x + (int)cells[i].off;

        if (cells[i].img)
            tray_blit(wm, mon, m, &cells[i], cx, y, box);
        else
            tray_placeholder(wm, mon, cx, y, cells[i].w, box);
    }
    return tray_width(cells, n);
}

/* Buttons 1/2/3 are Activate / SecondaryActivate / ContextMenu; 4/5 and
 * 6/7 are the wheel pairs, vertical and horizontal. The backend picks
 * the member and resolves the snapshot index, so this handler only maps
 * a button onto a cell.
 *
 * Button 3 reaches us because the tray row claims it
 * (mod_owns_right_click); there is no in-Austere menu in this phase -
 * the item is asked to show its own, and the popup arrives with the
 * DBusMenu work. */
static void
traymod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x, int px,
    unsigned btn)
{
    cell_t cells[TRAY_MAX_ITEMS];
    unsigned n = tray_layout(wm, mon, cells), hit;

    (void)m;                 /* a press needs no instance state */
    if (!n)
        return;
    /* px is relative to the module origin, so it indexes the cells
     * directly. The bar also hands us the trailing BAR_PAD it adds
     * between modules; that pad belongs to the nearest item rather than
     * to nobody. */
    hit = n - 1;
    for (unsigned i = 0; i < n; i++)
        if (px < (int)(cells[i].off + cells[i].w)) {
            hit = i;
            break;
        }
    switch (btn) {
    case XCB_BUTTON_INDEX_1:
    case XCB_BUTTON_INDEX_2:
    case XCB_BUTTON_INDEX_3: {
        unsigned box = tray_box(wm);
        int rx, ry;

        /* The press x is exact; the module contract carries no press y,
         * so the icon's own vertical center stands in for it - which is
         * where a host would place the item's menu anyway. */
        bar_root_point(mon, mod_x + px,
            tray_icon_y(mon, box) + (int)box / 2, &rx, &ry);
        tray_click(hit, btn, rx, ry);
        return;
    }
    case XCB_BUTTON_INDEX_4:                    /* wheel up */
        tray_scroll(hit, 1, false);
        return;
    case 5:                                     /* wheel down */
        tray_scroll(hit, -1, false);
        return;
    case 6:                                     /* wheel left */
        tray_scroll(hit, -1, true);
        return;
    case 7:                                     /* wheel right */
        tray_scroll(hit, 1, true);
        return;
    }
}

const mod_reg_t traymod = { "tray", traymod_render, traymod_click };

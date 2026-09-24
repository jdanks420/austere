#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/shape.h>

#include "bar.h"
#include "client.h"
#include "deco.h"
#include "draw.h"
#include "layout.h"
#include "monitor.h"
#include "mouse.h"
#include "settings.h"
#include "util.h"
#include "workspace.h"

#define DECO_PAD 4
#define DECO_ALPHA_MASK 0xd9000000 /* title strip pixel alpha (~85%) */

/* Nerd Font button glyphs (UTF-8 encoded); each is overridable through
 * [deco] buttons = ["<min>", "<max>", "<close>"] in the config, and an
 * empty entry keeps the built-in glyph. */
static const char GLYPH_MIN[] = "\xee\x80\x80";   /* U+E000  */
static const char GLYPH_MAX[] = "\xf3\xb0\x9d\xa4"; /* U+F0764 󰝤 */
static const char GLYPH_CLOSE[] = "\xf3\xb0\x9a\x8c"; /* U+F068C 󰚌 */

static const char *
btn_glyph(unsigned i)
{
    const char *want = i < 3 ? cfg.deco_buttons[i] : NULL;
    static const char *fallback[3] = { GLYPH_MIN, GLYPH_MAX, GLYPH_CLOSE };

    return (want && *want) ? want : fallback[i];
}

client_t *
find_client_by_deco(wm_t *wm, xcb_window_t win)
{
    for (client_t *c = wm->clients; c; c = c->next)
        if (c->deco && c->deco->win == win)
            return c;
    return NULL;
}

/* Find a 32-bit ARGB visual for compositor transparency (picom). NULL
 * when the screen offers no 32-bit depth. */
static xcb_visualtype_t *
find_argb_visual(xcb_screen_t *screen)
{
    xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);

    for (; di.rem; xcb_depth_next(&di)) {
        if (di.data->depth != 32)
            continue;
        xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);

        for (; vi.rem; xcb_visualtype_next(&vi)) {
            if (vi.data->_class == XCB_VISUAL_CLASS_DIRECT_COLOR ||
                vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
                return vi.data;
        }
    }
    return NULL;
}

/* Create the title-bar window. The ARGB variant is tried first so a
 * compositor can fade the strip; servers exist that advertise 32-bit
 * visuals and still refuse depth-32 windows (BadMatch), so the request
 * is checked and we fall back to the opaque root-visual surface instead
 * of leaving a window that nothing can ever draw to. False only when
 * even the opaque window is refused. */
static bool
deco_create_window(wm_t *wm, deco_t *d)
{
    uint16_t cls = XCB_WINDOW_CLASS_INPUT_OUTPUT;
    uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t events = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_void_cookie_t ck;

    if (d->transparent) {
        /* value lists follow ascending mask-bit significance:
         * BACK_PIXMAP < OVERRIDE_REDIRECT < EVENT_MASK < COLORMAP */
        mask |= XCB_CW_BACK_PIXMAP | XCB_CW_COLORMAP;
        uint32_t vals[4] = { XCB_BACK_PIXMAP_NONE, 1, events, d->cmap };

        ck = xcb_create_window_checked(wm->conn, wm->deco_argb_depth,
            d->win, wm->scr->root, (int16_t)d->win_x, (int16_t)d->win_y,
            (uint16_t)d->win_w, (uint16_t)d->win_h, 0, cls,
            wm->deco_argb_visual, mask, vals);
    } else {
        uint32_t vals[2] = { 1, events };

        ck = xcb_create_window_checked(wm->conn, wm->scr->root_depth,
            d->win, wm->scr->root, (int16_t)d->win_x, (int16_t)d->win_y,
            (uint16_t)d->win_w, (uint16_t)d->win_h, 0, cls,
            wm->scr->root_visual, mask, vals);
    }
    xcb_generic_error_t *e = xcb_request_check(wm->conn, ck);

    if (!e)
        return true;
    free(e);
    return false;
}

/* Gate ARGB title bars on a compositor actually owning _NET_WM_CM_S<n>:
 * only then do alpha pixels get blended. On bare servers (no compositor)
 * the opaque root-visual path is used and no 32-bit window is ever
 * attempted, so the broken-server cases can't occur. */
void
deco_init(wm_t *wm)
{
    char nm[24];

    snprintf(nm, sizeof(nm), "_NET_WM_CM_S%d", wm->scr_index);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(wm->conn,
        xcb_intern_atom(wm->conn, 0, (uint16_t)strlen(nm), nm), NULL);
    xcb_atom_t sel = r ? r->atom : XCB_ATOM_NONE;

    free(r);
    bool cm = false;

    if (sel != XCB_ATOM_NONE) {
        xcb_get_selection_owner_reply_t *o = xcb_get_selection_owner_reply(
            wm->conn, xcb_get_selection_owner(wm->conn, sel), NULL);

        cm = o && o->owner != XCB_NONE;
        free(o);
    }
    xcb_visualtype_t *vis = cm ? find_argb_visual(wm->scr) : NULL;

    wm->deco_argb = vis && vis->visual_id != wm->scr->root_visual;
    wm->deco_argb_depth = wm->deco_argb ? 32 : 0;
    wm->deco_argb_visual = wm->deco_argb ? vis->visual_id : 0;
}

/* A strip is only mapped while its client is actually on screen:
 * fullscreen owns the whole output, and minimized / scratch-hidden
 * clients are unmapped or parked. Mapping anyway is what leaves a stray
 * title bar painted over a fullscreen window when a config reload turns
 * decorations back on. */
static bool
deco_visible(const client_t *c)
{
    return !c->fullscreen && !c->minimized && !c->scratch_hidden;
}

/* Strip geometry: the bar sits directly on top of the client area,
 * clamped to the top of the client's own monitor so a window pulled
 * against the bar never rides underneath it. */
static void
deco_place(wm_t *wm, client_t *c, deco_t *d)
{
    monitor_t *m = workspaces[c->ws].mon;

    if (!m)
        m = focused_mon(wm);
    int min_y = m ? m->geom.y : 0;

    d->win_w = c->w;
    d->win_h = d->title_h;
    d->win_x = c->x;
    d->win_y = c->y - (int)d->title_h;
    if (d->win_y < min_y)
        d->win_y = min_y;
}

/* Add a standalone title bar above a managed client. The client itself
 * stays a direct child of the root: it is never reparented and never
 * covered by decoration, so a transparent application (an ARGB terminal,
 * a GTK client) keeps compositing straight against the desktop. Only
 * the strip carries per-pixel alpha. */
void
deco_create(wm_t *wm, client_t *c)
{
    if (c->deco)
        return;
    deco_t *d = xmalloc(sizeof(*d));

    memset(d, 0, sizeof(*d));
    d->title_h = cfg.deco_title_h;
    d->btn_size = d->title_h;
    d->win = xcb_generate_id(wm->conn);
    deco_place(wm, c, d);

    d->transparent = wm->deco_argb;
    if (d->transparent) {
        d->cmap = xcb_generate_id(wm->conn);
        xcb_create_colormap(wm->conn, XCB_COLORMAP_ALLOC_NONE, d->cmap,
            wm->scr->root, wm->deco_argb_visual);
        if (!deco_create_window(wm, d)) {
            /* server refused depth 32: keep the strip, drop the alpha */
            xcb_free_colormap(wm->conn, d->cmap);
            d->cmap = XCB_NONE;
            d->transparent = false;
            if (!deco_create_window(wm, d)) {
                free(d);
                return;
            }
        }
    } else if (!deco_create_window(wm, d)) {
        free(d);
        return;
    }
    draw_setup(wm, &d->draw, d->win);

    /* The strip replaces the frame: no server border on the client. */
    xcb_configure_window(wm->conn, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH,
        (uint32_t[]){ 0 });

    c->deco = d;
    mouse_grab_client(wm, d->win);
    if (deco_visible(c))
        xcb_map_window(wm->conn, d->win);
    deco_shape(wm, c);
    deco_draw(wm, c);
}

void
deco_destroy(wm_t *wm, client_t *c)
{
    deco_t *d = c->deco;

    if (!d)
        return;
    c->deco = NULL;
    /* Restore the real border width; the client never left the root. */
    xcb_configure_window(wm->conn, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH,
        (uint32_t[]){ cfg.border_width });
    deco_cleanup(wm, d);
}

void
deco_cleanup(wm_t *wm, deco_t *d)
{
    if (!d)
        return;
    xcb_destroy_window(wm->conn, d->win);
    if (d->cmap != XCB_NONE)
        xcb_free_colormap(wm->conn, d->cmap);
    xcb_free_gc(wm->conn, d->draw.gc);
    free(d);
}

/* Button layout: right-to-left: [close][maximize][minimize].
 * Rightmost button is flush against the right edge with DECO_PAD. */
static void
deco_compute_buttons(deco_t *d)
{
    d->close_x = (int)d->win_w - (int)d->btn_size - DECO_PAD;
    d->max_x = d->close_x - (int)d->btn_size;
    d->min_x = d->max_x - (int)d->btn_size;
    if (d->min_x < DECO_PAD)
        d->min_x = DECO_PAD;
    d->close_y = 0;
    d->max_y = 0;
    d->min_y = 0;
}

static void
deco_draw_title(wm_t *wm, client_t *c)
{
    deco_t *d = c->deco;

    if (!d)
        return;
    bool focused = wm->focused == c;
    uint32_t a = d->transparent ? DECO_ALPHA_MASK : 0;

    /* One constant face regardless of focus: glyphs, title, and bg pair
     * the same bar colors on every window. Focus shows only in the 1px
     * border, so nothing flips color as windows gain/lose focus. The
     * alpha byte rides on every strip pixel so a compositor fades the
     * title bar without touching the client behind it. */
    draw_rect(wm, &d->draw, 0, 0, d->win_w, d->title_h,
        cfg.bar_bg | a);

    /* Border frame around the strip. */
    uint32_t border = (focused ? cfg.deco_border
                               : cfg.deco_unfocus_border) | a;

    draw_rect(wm, &d->draw, 0, 0, d->win_w, 1, border);
    draw_rect(wm, &d->draw, 0, 0, 1, d->title_h, border);
    draw_rect(wm, &d->draw, (int)d->win_w - 1, 0, 1, d->title_h, border);
    draw_rect(wm, &d->draw, 0, (int)d->title_h - 1, d->win_w, 1, border);

    /* Title text, ellipsized to fit before the buttons. */
    font_t *f = draw_ui_font(wm);
    const char *name = c->name ? c->name : "";
    size_t len = strlen(name);
    unsigned maxw = d->min_x > (int)DECO_PAD ? (unsigned)d->min_x - DECO_PAD
                                              : 0;
    unsigned tw = draw_text_w(wm, f, name, len);

    while (tw > maxw && len > 0) {
        len--;
        tw = draw_text_w(wm, f, name, len);
    }
    if (len < strlen(name) && len > 0) {
        unsigned ew = draw_text_w(wm, f, "...", 3);

        while (len > 0 && tw + ew > maxw) {
            len--;
            tw = draw_text_w(wm, f, name, len);
        }
    }
    unsigned ascent = font_ascent(f);
    int baseline = DECO_PAD + (int)ascent;

    draw_text(wm, &d->draw, f, DECO_PAD, baseline, name, len,
        cfg.bar_fg | a, cfg.bar_bg | a);

    /* Button glyphs: minimize, maximize, close (right-to-left). */
    for (unsigned i = 0; i < 3; i++) {
        const char *g = btn_glyph(i);
        int gx = i == 0 ? d->min_x : (i == 1 ? d->max_x : d->close_x);

        draw_text(wm, &d->draw, f, gx, baseline, g, strlen(g),
            cfg.bar_fg | a, cfg.bar_bg | a);
    }
}

void
deco_draw(wm_t *wm, client_t *c)
{
    if (!c->deco || !deco_visible(c))
        return;
    deco_compute_buttons(c->deco);
    deco_draw_title(wm, c);
}

/* Round the strip to match corner_radius. Only the strip is shaped here:
 * the client is no longer a child of it, so the client's own pixels are
 * never clipped and it keeps its independent inner rounding (§5.7). */
void
deco_shape(wm_t *wm, client_t *c)
{
    deco_t *d = c->deco;

    if (!d)
        return;
    unsigned radius = (c->fullscreen || cfg.corner_radius == 0)
        ? 0 : cfg.corner_radius;

    if (radius == 0) {
        xcb_rectangle_t full = { 0, 0, (uint16_t)d->win_w,
            (uint16_t)d->win_h };

        xcb_shape_rectangles(wm->conn, XCB_SHAPE_SO_SET,
            XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, d->win,
            0, 0, 1, &full);
        return;
    }
    shape_window(wm, d->win, d->win_w, d->win_h, radius);
}

/* Keep the strip glued to the client-area rect stored in c->x/y/w/h by
 * apply_geom. Fullscreen owns the whole output, so the strip unmaps and
 * the borderless client covers the monitor. */
void
deco_update(wm_t *wm, client_t *c)
{
    deco_t *d = c->deco;

    if (!d)
        return;
    if (!deco_visible(c)) {
        xcb_unmap_window(wm->conn, d->win);
        return;
    }
    deco_place(wm, c, d);
    uint32_t vals[4] = { (uint32_t)d->win_x, (uint32_t)d->win_y,
        d->win_w, d->win_h };

    xcb_configure_window(wm->conn, d->win,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        vals);
    xcb_map_window(wm->conn, d->win);
    deco_shape(wm, c);
    deco_draw(wm, c);
}

void
deco_toggle_maximize(wm_t *wm, client_t *c)
{
    deco_t *d = c->deco;

    if (!d)
        return;
    monitor_t *m = focused_mon(wm);

    if (!m)
        return;
    if (!d->maximized) {
        d->orig_x = c->x;
        d->orig_y = c->y;
        d->orig_w = c->w;
        d->orig_h = c->h;
        d->prev_floating = c->floating;
        c->floating = true;
        Rect wa = mon_workarea(m);
        unsigned th = d->title_h;

        /* Reserve the strip's own height, exactly as the tiled layouts
         * do: the title bar then lands inside the work area instead of
         * riding over the bar. */
        apply_geom(wm, c, wa.x, wa.y + (int)th, (unsigned)wa.w,
            (unsigned)wa.h > th ? (unsigned)wa.h - th : 1);
        d->maximized = true;
    } else {
        d->maximized = false;
        c->floating = d->prev_floating;
        apply_geom(wm, c, d->orig_x, d->orig_y, d->orig_w, d->orig_h);
    }
    arrange(wm);
    focus(wm, c);
}

/* Hide a decorated window by unmapping both the strip and the client.
 * Tracked via c->minimized so the workspace title can hint at hidden
 * clients. */
void
deco_minimize(wm_t *wm, client_t *c)
{
    deco_t *d = c->deco;

    if (!d)
        return;
    d->orig_x = c->x;
    d->orig_y = c->y;
    d->orig_w = c->w;
    d->orig_h = c->h;
    c->minimized = true;
    xcb_unmap_window(wm->conn, d->win);
    xcb_unmap_window(wm->conn, c->win);
    if (wm->focused == c) {
        monitor_t *m = workspaces[c->ws].mon;

        if (m)
            refocus_ws(wm, m->ws_visible);
    }
    arrange(wm);
}

/* Restore the most recently minimized client (newest first in the
 * client list): re-map and focus it on its home workspace. */
void
deco_restore_minimized(wm_t *wm)
{
    for (client_t *c = wm->clients; c; c = c->next) {
        if (!c->minimized)
            continue;
        if (!c->deco)
            continue;
        deco_t *d = c->deco;

        c->minimized = false;
        monitor_t *m = workspaces[c->ws].mon;

        if (m && m->ws_visible != c->ws)
            view_ws(wm, c->ws);
        if (c->floating)
            apply_geom(wm, c, d->orig_x, d->orig_y, d->orig_w,
                d->orig_h);
        apply_geom(wm, c, c->x, c->y, c->w, c->h);
        xcb_map_window(wm->conn, d->win);
        xcb_map_window(wm->conn, c->win);
        arrange(wm);
        focus(wm, c);
        return;
    }
}

/* Route clicks on a title bar. Returns true if consumed. */
bool
deco_button_hit(wm_t *wm, xcb_button_press_event_t *ev, unsigned btn)
{
    client_t *c = find_client_by_deco(wm, ev->event);

    if (!c)
        return false;
    deco_t *d = c->deco;
    int x = ev->event_x;
    int y = ev->event_y;

    deco_compute_buttons(d);

    if (y < (int)d->title_h) {
        if (btn == 1) {
            if (x >= d->close_x &&
                x < d->close_x + (int)d->btn_size) {
                client_close(wm, c);
                /* ASYNC drops the frozen press. Replaying it would
                 * redeliver into the strip's own BUTTON_PRESS mask
                 * and fire the action twice. */
                xcb_allow_events(wm->conn, XCB_ALLOW_ASYNC_POINTER,
                    ev->time);
            } else if (x >= d->max_x &&
                x < d->max_x + (int)d->btn_size) {
                deco_toggle_maximize(wm, c);
                xcb_allow_events(wm->conn, XCB_ALLOW_ASYNC_POINTER,
                    ev->time);
            } else if (x >= d->min_x &&
                x < d->min_x + (int)d->btn_size) {
                deco_minimize(wm, c);
                xcb_allow_events(wm->conn, XCB_ALLOW_ASYNC_POINTER,
                    ev->time);
            } else {
                focus(wm, c);
                move_drag_begin(wm, c, ev);
                /* Button1 on the strip is a SYNC grab: release the
                 * freeze AND hand pointer control to our active grab so
                 * xcb_grab_pointer (in begin_drag) receives motion. */
                xcb_allow_events(wm->conn, XCB_ALLOW_ASYNC_POINTER,
                    ev->time);
            }
        } else {
            xcb_allow_events(wm->conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
        }
        return true;
    }
    return false;
}

void
deco_reconfigure_all(wm_t *wm)
{
    for (client_t *c = wm->clients; c; c = c->next) {
        if (cfg.deco && !c->deco && !c->scratchpad)
            deco_create(wm, c);
        else if (!cfg.deco && c->deco)
            deco_destroy(wm, c);
    }
}

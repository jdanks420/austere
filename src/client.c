#define _GNU_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "bar.h"
#include "settings.h"
#include "client.h"
#include "monitor.h"
#include "ewmh.h"
#include "layout.h"
#include "mouse.h"
#include "util.h"
#include "workspace.h"

static client_t *swallow_victim_find(wm_t *wm, client_t *c);

client_t *
find_client(wm_t *wm, xcb_window_t win)
{
    for (client_t *c = wm->clients; c; c = c->next)
        if (c->win == win)
            return c;
    return NULL;
}

static void
read_hints(wm_t *wm, client_t *c, bool *self_positioned)
{
    xcb_size_hints_t hints;

    *self_positioned = false;
    if (xcb_icccm_get_wm_normal_hints_reply(wm->conn,
            xcb_icccm_get_wm_normal_hints(wm->conn, c->win), &hints,
            NULL)) {
        if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
            c->min_w = hints.min_width;
            c->min_h = hints.min_height;
        }
        /* ICCCM: PBaseSize/PMinSize with fixed aspect implies non-resizable
         * windows (dialogs, clocks) float rather than tile. */
        c->floating = c->floating ||
            ((hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
                hints.min_width == hints.max_width);
        /* base + increment: cell-grid apps (kitty) size themselves as
         * base + n*inc and overshoot the WM's geometry without this */
        if (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
            c->base_w = hints.base_width;
            c->base_h = hints.base_height;
        } else if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
            c->base_w = hints.min_width;
            c->base_h = hints.min_height;
        }
        if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
            c->inc_w = hints.width_inc;
            c->inc_h = hints.height_inc;
        }
        *self_positioned =
            (hints.flags &
                (XCB_ICCCM_SIZE_HINT_P_POSITION |
                    XCB_ICCCM_SIZE_HINT_US_POSITION)) != 0;
    }
}

#define FOCUS_COLOR (cfg.focus_color)
#define UNFOCUS_COLOR (cfg.unfocus_color)
#define URGENT_COLOR (cfg.urgent_color)

void
set_border(wm_t *wm, client_t *c, unsigned long color)
{
    xcb_change_window_attributes(wm->conn, c->win, XCB_CW_BORDER_PIXEL,
        (uint32_t[]){ color });
}

void
manage(wm_t *wm, xcb_window_t win)
{
    atoms_t *a = wm->atoms;
    xcb_get_geometry_reply_t *geo;
    client_t *c;
    size_t len;
    char *name;
    bool self_positioned;
    int geo_x = 0, geo_y = 0;

    if (find_client(wm, win))
        return;

    c = xmalloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->win = win;
    monitor_t *fm = focused_mon(wm);

    c->ws = fm ? fm->ws_visible : 0;

    geo = xcb_get_geometry_reply(wm->conn, xcb_get_geometry(wm->conn, win),
        NULL);
    if (geo) {
        c->w = geo->width;
        c->h = geo->height;
        geo_x = geo->x;
        geo_y = geo->y;
    } else {
        c->w = 640;
        c->h = 480;
    }
    free(geo);

    name = (char *)get_string_property(wm, win, a->net_wm_name, &len);
    if (!name)
        name = (char *)get_property(wm, win, XCB_ATOM_WM_NAME,
            XCB_ATOM_STRING, 8, &len);
    c->name = name ? name : xstrdup("");

    c->pid = 0;
    {
        size_t plen = 0;
        void *pv = get_property(wm, win, a->net_wm_pid, XCB_ATOM_CARDINAL,
            32, &plen);

        if (pv && plen >= 1)
            c->pid = *(pid_t *)pv;
        free(pv);
    }
    c->cls = NULL;
    {
        size_t clen = 0;
        char *raw = (char *)get_property(wm, win, XCB_ATOM_WM_CLASS,
            XCB_ATOM_STRING, 8, &clen);

        if (raw && clen > 1) {
            char *cls_part = raw + strlen(raw) + 1 < raw + clen
                ? raw + strlen(raw) + 1
                : raw;

            c->cls = xstrdup(cls_part);
        }
        free(raw);
    }

    /* Transient dialogs float (ICCCM §4.2.3 + SPEC §5.3). */
    {
        xcb_window_t transient = XCB_NONE;
        size_t tlen;
        void *tv = get_property(wm, win, XCB_ATOM_WM_TRANSIENT_FOR,
            XCB_ATOM_WINDOW, 32, &tlen);
        if (tv && tlen >= sizeof(transient))
            memcpy(&transient, tv, sizeof(transient));
        free(tv);
        if (transient != XCB_NONE && transient != wm->scr->root)
            c->floating = true;
    }

    read_hints(wm, c, &self_positioned);

    /* Windows with no placement opinion get a cascade slot so they don't
     * all pile up at the server default position. */
    if (!self_positioned) {
        int off = 24 * (int)(wm->nclients % 10 + 1);

        c->x = off;
        c->y = off;
    } else {
        c->x = geo_x;
        c->y = geo_y;
    }

    xcb_change_window_attributes(wm->conn, win,
        XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK,
        (uint32_t[]){ UNFOCUS_COLOR,
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_PROPERTY_CHANGE });
    xcb_configure_window(wm->conn, win, XCB_CONFIG_WINDOW_BORDER_WIDTH,
        (uint32_t[]){ cfg.border_width });
    mouse_grab_client(wm, win);

    c->next = wm->clients;
    if (wm->clients)
        wm->clients->prev = c;
    wm->clients = c;
    wm->nclients++;

    xcb_map_window(wm->conn, win);
    arrange(wm);
    ewmh_update_client_list(wm);
    ewmh_update_wm_desktop(wm, c);
    client_poll_urgency(wm, c);
    focus(wm, c);

    /* §5.9: a GUI child launched from a managed terminal takes the
     * terminal's slot until it exits. */
    client_t *victim = swallow_victim_find(wm, c);

    if (victim) {
        victim->swallowed_by = c;
        c->swallow_victim = victim;
        victim->scratch_hidden = true;
        client_park(wm, victim, true);
        arrange(wm);
    }
}

/* §5.9: nearest managed terminal-class ancestor of pid, within the
 * same workspace. Depth-capped PPid walk through /proc. */

void
unmanage(wm_t *wm, xcb_window_t win)
{
    client_t *c = find_client(wm, win);

    if (!c)
        return;
    unsigned ws_idx = c->ws;
    workspace_t *ws = &workspaces[ws_idx];
    if (ws->sel == c)
        ws->sel = NULL;
    if (c == wm->focused)
        wm->focused = NULL;
    if (c->prev)
        c->prev->next = c->next;
    if (c->next)
        c->next->prev = c->prev;
    if (wm->clients == c)
        wm->clients = c->next;
    wm->nclients--;

    xcb_delete_property(wm->conn, win, wm->atoms->net_wm_desktop);

    free(c->name);
    free(c->cls);
    if (c->swallow_victim) {
        client_t *v = c->swallow_victim;

        v->swallowed_by = NULL;
        c->swallow_victim = NULL;
        v->scratch_hidden = false;
        xcb_get_window_attributes_reply_t *wa =
            xcb_get_window_attributes_reply(wm->conn,
                xcb_get_window_attributes(wm->conn, v->win), NULL);

        if (wa) { /* exists: any map state is restorable */
            client_park(wm, v, false);
        }
        free(wa);
        refocus_ws(wm, workspaces[v->ws].mon
                ? workspaces[v->ws].mon->ws_visible
                : v->ws);
    }
    if (c->swallowed_by)
        c->swallowed_by->swallow_victim = NULL;
    free(c);
    ws_recompute_urgent(wm, ws_idx);
    ewmh_update_client_list(wm);

    /* Only a shown workspace needs a new focus and geometry; touching
     * focus for a hidden one would clear it globally (focus_mon=NULL). */
    monitor_t *mon = workspaces[ws_idx].mon;

    if (mon && mon->ws_visible == ws_idx) {
        refocus_ws(wm, mon->ws_visible);
        arrange(wm);
    }
}

/* Re-read the title property after a PropertyNotify; NULL-safe swap. */
static client_t *
swallow_victim_find(wm_t *wm, client_t *c)
{
    if (!cfg.swallowing || !c->pid)
        return NULL;
    pid_t p = c->pid;

    for (int depth = 0; depth < 10 && p > 1; depth++) {
        char ppath[64];

        snprintf(ppath, sizeof(ppath), "/proc/%d/status", (int)p);
        FILE *f = fopen(ppath, "r");

        if (!f)
            break;
        char line[128];
        pid_t ppid = 0;

        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "PPid:%d", &ppid) == 1)
                break;
        }
        fclose(f);

        for (client_t *t = wm->clients; t; t = t->next) {
            if (t == c || t->pid != p || t->scratchpad ||
                t->scratch_hidden || t->swallowed_by)
                continue;
            if (t->ws != c->ws)
                continue;
            if (!t->cls)
                continue;
            if (strcasestr(t->cls, "term"))
                return t;
            /* common terminal classes without the "term" substring */
            static const char *const terms[] = { "kitty", "alacritty",
                "st", "st-256color", "foot", "urxvt", "konsole", NULL };

            for (unsigned k = 0; terms[k]; k++)
                if (!strcasecmp(t->cls, terms[k]))
                    return t;
        }
        p = ppid;
    }
    return NULL;
}

void
client_refresh_name(wm_t *wm, client_t *c)
{
    atoms_t *a = wm->atoms;
    size_t len;
    char *name = (char *)get_string_property(wm, c->win, a->net_wm_name,
        &len);

    if (!name)
        name = (char *)get_property(wm, c->win, XCB_ATOM_WM_NAME,
            XCB_ATOM_STRING, 8, &len);
    if (!name)
        return;
    free(c->name);
    c->name = name;
}

/* §5.7: rounded bounding box via XShape, approximated with one
 * rectangle per corner scanline. radius 0 clears the shape. Period-
 * correct hard-edged rounding: no anti-aliasing, ever. */
void
client_shape(wm_t *wm, client_t *c, unsigned radius)
{
    if (radius == 0) {
        xcb_shape_rectangles(wm->conn, XCB_SHAPE_SO_SET,
            XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, c->win,
            0, 0, 0, NULL);
        return;
    }
    unsigned w = c->w, h = c->h;

    if (w < radius * 2 || h < radius * 2)
        radius = (w < h ? w : h) / 2;
    if (radius == 0)
        return;
    xcb_rectangle_t rects[64];
    unsigned n = 0;

    for (unsigned i = 0; i < radius && n < 64; i++) {
        unsigned dy = radius - i;
        unsigned dx = radius -
            (unsigned)sqrt((double)dy * (double)dy);

        if (n < 64)
            rects[n++] = (xcb_rectangle_t){
                (int16_t)(radius - dx), (int16_t)i,
                (uint16_t)(w - 2 * (radius - dx)), 1 };
    }
    if (n < 64)
        rects[n++] = (xcb_rectangle_t){ 0, (int16_t)radius, (uint16_t)w,
            (uint16_t)(h - 2 * radius) };
    for (unsigned i = 0; i < radius && n < 64; i++) {
        unsigned dy = i + 1;
        unsigned dx = radius -
            (unsigned)sqrt((double)dy * (double)dy);

        if (n < 64)
            rects[n++] = (xcb_rectangle_t){
                (int16_t)(radius - dx), (int16_t)(h - radius + i),
                (uint16_t)(w - 2 * (radius - dx)), 1 };
    }
    xcb_shape_rectangles(wm->conn, XCB_SHAPE_SO_SET,
        XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, c->win, 0,
        0, (uint16_t)n, rects);
}

void
apply_geom(wm_t *wm, client_t *c, int x, int y, unsigned w, unsigned h)
{
    monitor_t *home = workspaces[c->ws].mon;

    /* cell-grid apps (kitty) size themselves base + n*inc; rounding
     * DOWN keeps the window inside whatever the layout asked for */
    if (c->inc_w > 0 && w > (unsigned)c->base_w)
        w = (unsigned)(c->base_w +
            (int)(w - c->base_w) / c->inc_w * c->inc_w);
    if (c->inc_h > 0 && h > (unsigned)c->base_h)
        h = (unsigned)(c->base_h +
            (int)(h - c->base_h) / c->inc_h * c->inc_h);
    /* never exceed the monitor: a cell overshoot must not hang
     * off-screen */
    if (home) {
        if (w > home->geom.w)
            w = home->geom.w;
        if (h > home->geom.h)
            h = home->geom.h;
    }

    if (c->fullscreen && home) {
        /* EWMH fullscreen: the whole output, borderless, unshaped */
        x = home->geom.x;
        y = home->geom.y;
        w = home->geom.w;
        h = home->geom.h;
        xcb_configure_window(wm->conn, c->win,
            XCB_CONFIG_WINDOW_BORDER_WIDTH, (uint32_t[]){ 0 });
        client_shape(wm, c, 0);
    } else if (cfg.corner_radius > 0) {
        xcb_configure_window(wm->conn, c->win,
            XCB_CONFIG_WINDOW_BORDER_WIDTH,
            (uint32_t[]){ cfg.border_width });
        client_shape(wm, c, cfg.corner_radius);
    }

    uint32_t vals[4] = { (uint32_t)x, (uint32_t)y, w, h };

    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    xcb_configure_window(wm->conn, c->win,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
            XCB_CONFIG_WINDOW_HEIGHT,
        vals);
    xcb_send_event(wm->conn, 0, c->win, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
        (const char *)&(xcb_configure_notify_event_t){
            .response_type = XCB_CONFIGURE_NOTIFY,
            .event = c->win,
            .window = c->win,
            .x = (int16_t)x,
            .y = (int16_t)y,
            .width = (uint16_t)w,
            .height = (uint16_t)h,
            .border_width = (uint16_t)cfg.border_width,
        });
}

void
client_poll_urgency(wm_t *wm, client_t *c)
{
    xcb_icccm_wm_hints_t hints;

    if (xcb_icccm_get_wm_hints_reply(wm->conn,
            xcb_icccm_get_wm_hints(wm->conn, c->win), &hints, NULL) &&
        (hints.flags & XCB_ICCCM_WM_HINT_X_URGENCY))
        client_set_urgent(wm, c, true);
}

void
focus(wm_t *wm, client_t *c)
{
    atoms_t *a = wm->atoms;

    if (c && c->scratch_hidden)
        c = NULL;
    if (c && wm->focused == c)
        return;
    if (wm->focused)
        set_border(wm, wm->focused, UNFOCUS_COLOR);
    wm->focused = c;
    if (c) {
        if (workspaces[c->ws].mon)
            wm->focus_mon = workspaces[c->ws].mon;
        set_border(wm, c,
            c->urgent ? URGENT_COLOR : FOCUS_COLOR);
        xcb_set_input_focus(wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->win,
            XCB_CURRENT_TIME);
        workspaces[c->ws].sel = c;
        if (c->urgent)
            client_set_urgent(wm, c, false);
    } else {
        xcb_set_input_focus(wm->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
            wm->scr->root, XCB_CURRENT_TIME);
    }
    bar_render_all(wm);

    uint32_t active = c ? c->win : XCB_NONE;
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        a->net_active_window, XCB_ATOM_WINDOW, 32, 1, &active);
}

void
focus_clear(wm_t *wm)
{
    focus(wm, NULL);
}

void
refocus_ws(wm_t *wm, unsigned idx)
{
    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws == idx && !c->scratch_hidden) {
            focus(wm, c);
            return;
        }
    }
    if (idx < WS_MAX)
        wm->focus_mon = workspaces[idx].mon;
    focus_clear(wm);
}

void
client_set_urgent(wm_t *wm, client_t *c, bool urgent)
{
    if (c->urgent == urgent)
        return;
    if (urgent && wm->focused == c && !c->scratch_hidden &&
        ws_shown(c->ws))
        return;
    c->urgent = urgent;
    ewmh_set_demands_attention(wm, c, urgent);
    ws_recompute_urgent(wm, c->ws);
    if (!c->scratch_hidden && ws_shown(c->ws))
        set_border(wm, c,
            urgent ? URGENT_COLOR :
                (wm->focused == c ? FOCUS_COLOR : UNFOCUS_COLOR));
}

void
ws_recompute_urgent(wm_t *wm, unsigned idx)
{
    bool any = false;

    for (client_t *c = wm->clients; c; c = c->next)
        if (c->ws == idx && c->urgent)
            any = true;
    workspaces[idx].urgent = any;
}

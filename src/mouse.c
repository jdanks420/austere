#define _GNU_SOURCE

#include <xcb/xcb.h>

#include "client.h"
#include "layout.h"
#include "bar.h"
#include "settings.h"
#include "mouse.h"
#include <stdlib.h>
#include "util.h"

/* Passive grabs on every managed window: super+Btn1 moves, super+Btn3
 * resizes. Neither ever changes a window's floating state (i3 parity,
 * SPEC §5.4): floating clients move/resize freely, tiled clients refuse
 * move and super+Btn3 adjusts the tiling boundary instead. The un-modded
 * Btn1 grab is SYNC so click-to-focus can replay the press into the
 * application instead of swallowing it. */
void
mouse_grab_client(wm_t *wm, xcb_window_t win)
{
    const struct {
        uint8_t button;
        uint16_t mods;
        bool sync;
    } grabs[] = {
        { XCB_BUTTON_INDEX_1, 0, true },
        { XCB_BUTTON_INDEX_1, (uint16_t)cfg.mouse_mods, false },
        { XCB_BUTTON_INDEX_3, (uint16_t)cfg.mouse_mods, false },
    };

    for (size_t i = 0; i < 3; i++)
        xcb_grab_button(wm->conn, 0, win,
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
            grabs[i].sync ? XCB_GRAB_MODE_SYNC : XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, grabs[i].button,
            grabs[i].mods);
}

static void
begin_drag(wm_t *wm, client_t *c, xcb_button_press_event_t *ev,
    drag_mode_t mode)
{
    mouse_t *m = &wm->mouse;
    const layout_t *lay = &austere_layouts[wm->layout_idx];
    bool tiled = !c->floating && !lay->floats_all;

    if (tiled) {
        if (mode == DRAG_RESIZE && lay->ratio_aware)
            mode = DRAG_TILE_RESIZE;
        else {
            /* i3 parity (SPEC §5.4): modifier drags never float a tiled
             * window. Move, and resize in non-ratio layouts, are refused;
             * float it explicitly with toggle_float first. */
            focus(wm, c);
            if (cfg.raise_on_click)
                raise_client(wm, c);
            return;
        }
    }

    m->mode = mode;
    m->drag = c;
    m->press_x = ev->root_x;
    m->press_y = ev->root_y;
    m->orig_x = c->x;
    m->orig_y = c->y;
    m->orig_w = c->w;
    m->orig_h = c->h;

    if (mode == DRAG_TILE_RESIZE) {
        workspace_t *ws = &workspaces[c->ws];
        monitor_t *mm = ws->mon ? ws->mon : focused_mon(wm);
        Rect area = mon_workarea(mm);
        int scale = (int)area.w - 2 * (int)cfg.gap;

        m->tile_ratio = ws->split_ratio;
        m->tile_scale = scale > 0 ? scale : 1;
    }

    focus(wm, c);
    raise_client(wm, c);
    xcb_grab_pointer_cookie_t ck = xcb_grab_pointer(wm->conn, 0,
        wm->scr->root,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
        ev->time);
    xcb_grab_pointer_reply_t *r = xcb_grab_pointer_reply(wm->conn, ck,
        NULL);
    bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;

    free(r);
    /* A failed grab (another client owns the server grab) leaves us
     * with a phantom drag otherwise: no motion will arrive while we
     * keep a stale mode that swallows clicks. */
    if (!ok) {
        m->mode = DRAG_NONE;
        return;
    }
}

void
mouse_press(wm_t *wm, xcb_button_press_event_t *ev)
{
    client_t *c = find_client(wm, ev->event);

    if (!c || wm->mouse.mode != DRAG_NONE) {
        /* The un-modded button was frozen by the SYNC grab; always
         * replay or the server holds the pointer forever and every
         * future click is dead until X restarts. */
        xcb_allow_events(wm->conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        return;
    }
    if ((ev->state & cfg.mouse_mods) &&
        ev->detail == cfg.move_button)
        begin_drag(wm, c, ev, DRAG_MOVE);
    else if ((ev->state & cfg.mouse_mods) &&
        ev->detail == cfg.resize_button)
        begin_drag(wm, c, ev, DRAG_RESIZE);
    else {
        focus(wm, c);
        if (cfg.raise_on_click)
            raise_client(wm, c);
        /* The plain-button grab froze the pointer; replay delivers the
         * press to the application beneath it. */
        xcb_allow_events(wm->conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
    }
}

/* Pointer-identity membership test: safe to call even when `c` has
 * already been freed, so a stale drag target can be detected without
 * ever dereferencing it. */
static bool
client_still_managed(wm_t *wm, const client_t *c)
{
    for (const client_t *p = wm->clients; p; p = p->next)
        if (p == c)
            return true;
    return false;
}

void
mouse_motion(wm_t *wm, xcb_motion_notify_event_t *ev)
{
    mouse_t *m = &wm->mouse;
    client_t *c = m->drag;
    int dx, dy;

    if (!c || m->mode == DRAG_NONE)
        return;
    if (!client_still_managed(wm, c)) {
        /* The dragged window died mid-drag (killed/unmanaged): drop the
         * stale target and the active pointer grab before touching it. */
        m->mode = DRAG_NONE;
        m->drag = NULL;
        xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
        return;
    }
    dx = ev->root_x - m->press_x;
    dy = ev->root_y - m->press_y;

    if (m->mode == DRAG_MOVE) {
        int x = m->orig_x + dx;
        int y = m->orig_y + dy;

        /* Allow moving past screen edges (i3/dwm behavior). */
        apply_geom(wm, c, x, y, m->orig_w, m->orig_h);
    } else if (m->mode == DRAG_TILE_RESIZE) {
        /* Horizontal drag slides the master/stack boundary; the ratio
         * maps linearly across the tiling width and clamps like the
         * ratio_shrink/ratio_grow binds (adjust_mwfact). */
        double ratio = m->tile_ratio +
            (double)dx / (double)m->tile_scale;

        if (ratio < 0.1)
            ratio = 0.1;
        if (ratio > 0.9)
            ratio = 0.9;
        workspaces[c->ws].split_ratio = ratio;
        arrange(wm);
    } else {
        unsigned min_w = c->min_w ? c->min_w : 1;
        unsigned min_h = c->min_h ? c->min_h : 1;
        unsigned max_w = wm->scr->width_in_pixels;
        unsigned max_h = wm->scr->height_in_pixels;
        unsigned w = (unsigned)((int)m->orig_w + dx);
        unsigned h = (unsigned)((int)m->orig_h + dy);

        if (w < min_w)
            w = min_w;
        if (h < min_h)
            h = min_h;
        if (w > max_w)
            w = max_w;
        if (h > max_h)
            h = max_h;
        apply_geom(wm, c, m->orig_x, m->orig_y, w, h);
    }
}

void
mouse_release(wm_t *wm, xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm->mouse.mode == DRAG_NONE)
        return;
    wm->mouse.mode = DRAG_NONE;
    wm->mouse.drag = NULL;
    xcb_ungrab_pointer(wm->conn, XCB_CURRENT_TIME);
}

/* Title-bar drag entry: move the window without a modifier. The caller
 * (deco.c) has already focused the client; this just starts the drag. */
void
move_drag_begin(wm_t *wm, client_t *c, xcb_button_press_event_t *ev)
{
    begin_drag(wm, c, ev, DRAG_MOVE);
}

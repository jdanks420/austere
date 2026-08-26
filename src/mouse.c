#define _GNU_SOURCE

#include <xcb/xcb.h>

#include "client.h"
#include "layout.h"
#include "settings.h"
#include "mouse.h"
#include "util.h"

/* Passive grabs on every managed window: super+Btn1 moves, super+Btn3
 * resizes. The un-modded Btn1 grab is SYNC so click-to-focus can replay
 * the press into the application instead of swallowing it. */
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

    m->mode = mode;
    m->drag = c;
    m->press_x = ev->root_x;
    m->press_y = ev->root_y;
    m->orig_x = c->x;
    m->orig_y = c->y;
    m->orig_w = c->w;
    m->orig_h = c->h;

    if (!c->floating) {
        c->floating = true;
        arrange(wm);
    }

    focus(wm, c);
    xcb_configure_window(wm->conn, c->win, XCB_CONFIG_WINDOW_STACK_MODE,
        (uint32_t[]){ XCB_STACK_MODE_ABOVE });
    xcb_grab_pointer(wm->conn, 0, wm->scr->root,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
        ev->time);
}

static void
snap_to_edges(wm_t *wm, int *x, int *y, unsigned w, unsigned h)
{
    xcb_screen_t *s = wm->scr;
    int sw = (int)s->width_in_pixels;
    int sh = (int)s->height_in_pixels;

    /* Crossing an edge pins the window there for the rest of the drag. */
    if (*x < 0)
        *x = 0;
    else if (*x + (int)w > sw)
        *x = sw - (int)w;
    if (*y < 0)
        *y = 0;
    else if (*y + (int)h > sh)
        *y = sh - (int)h;
}

void
mouse_press(wm_t *wm, xcb_button_press_event_t *ev)
{
    client_t *c = find_client(wm, ev->event);

    if (!c || wm->mouse.mode != DRAG_NONE)
        return;
    if ((ev->state & cfg.mouse_mods) &&
        ev->detail == cfg.move_button)
        begin_drag(wm, c, ev, DRAG_MOVE);
    else if ((ev->state & cfg.mouse_mods) &&
        ev->detail == cfg.resize_button)
        begin_drag(wm, c, ev, DRAG_RESIZE);
    else {
        focus(wm, c);
        if (cfg.raise_on_click)
            xcb_configure_window(wm->conn, c->win,
                XCB_CONFIG_WINDOW_STACK_MODE,
                (uint32_t[]){ XCB_STACK_MODE_ABOVE });
        /* The plain-button grab froze the pointer; replay delivers the
         * press to the application beneath it. */
        xcb_allow_events(wm->conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
    }
}

void
mouse_motion(wm_t *wm, xcb_motion_notify_event_t *ev)
{
    mouse_t *m = &wm->mouse;
    client_t *c = m->drag;
    int dx, dy;

    if (!c || m->mode == DRAG_NONE)
        return;
    dx = ev->root_x - m->press_x;
    dy = ev->root_y - m->press_y;

    if (m->mode == DRAG_MOVE) {
        int x = m->orig_x + dx;
        int y = m->orig_y + dy;

        snap_to_edges(wm, &x, &y, c->w, c->h);
        apply_geom(wm, c, x, y, c->w, c->h);
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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>

#include "client.h"
#include "ewmh.h"
#include "layout.h"
#include "monitor.h"
#include "scratchpad.h"
#include "util.h"
#include "workspace.h"

void
scratch_mark(wm_t *wm)
{
    client_t *old = NULL;
    client_t *c = wm->focused;

    if (!c)
        return;
    if (c->scratchpad) {
        c->scratchpad = false;
        c->scratch_hidden = false;
        arrange(wm);
        return;
    }
    for (client_t *o = wm->clients; o; o = o->next) {
        if (o->scratchpad) {
            old = o;
            break;
        }
    }
    if (old) {
        /* The role is singular: the previous holder returns to its own
         * workspace as a normal floating client (SPEC §5.5). */
        old->scratchpad = false;
        if (!old->scratch_hidden && !ws_shown(old->ws)) {
            client_park(wm, old, true);
        } else if (ws_shown(old->ws)) {
            client_park(wm, old, false);
        }
        old->scratch_hidden = false;
    }
    c->scratchpad = true;
    c->floating = true;
    arrange(wm);
    focus(wm, c);
}

static void
show_scratch(wm_t *wm, client_t *c)
{
    monitor_t *m = focused_mon(wm);

    if (!m)
        return;
    unsigned w = c->w ? c->w : 640;
    unsigned h = c->h ? c->h : 480;

    if (w > m->geom.w)
        w = m->geom.w;
    if (h > m->geom.h)
        h = m->geom.h;
    c->x = m->geom.x + (int)(m->geom.w - w) / 2;
    c->y = m->geom.y + (int)(m->geom.h - h) / 2;
    /* Clear the hidden flag BEFORE the geometry apply: that is what
     * decides whether the decoration strip is mapped alongside the
     * client, so flipping it afterwards would leave the strip behind. */
    c->scratch_hidden = false;
    apply_geom(wm, c, c->x, c->y, w, h);
    xcb_map_window(wm->conn, c->win);
    raise_client(wm, c);
    focus(wm, c);
}

static void
hide_scratch(wm_t *wm, client_t *c)
{
    c->scratch_hidden = true;
    client_park(wm, c, true);
    if (wm->focused == c) {
        monitor_t *m = workspaces[c->ws].mon;

        if (m)
            refocus_ws(wm, m->ws_visible);
    }
}

void
scratch_toggle(wm_t *wm)
{
    client_t *c = NULL;

    for (client_t *o = wm->clients; o; o = o->next)
        if (o->scratchpad) {
            c = o;
            break;
        }
    if (!c)
        return; /* spawn-from-conf arrives with M7's [general].terminal */
    if (c->scratch_hidden)
        show_scratch(wm, c);
    else
        hide_scratch(wm, c);
}

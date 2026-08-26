#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "ewmh.h"
#include "layout.h"
#include <xcb/xcb_icccm.h>
#include "monitor.h"
#include "settings.h"
#include "util.h"
#include "workspace.h"

workspace_t workspaces[WS_MAX];

void
workspaces_init(wm_t *wm)
{
    memset(workspaces, 0, sizeof(workspaces));
    monitors_init(wm);

    for (unsigned i = 0; i < WS_MAX; i++) {
        workspaces[i].name =
            xstrdup(cfg.ws_names[i] && *cfg.ws_names[i]
                    ? cfg.ws_names[i]
                    : (char[]){ (char)('1' + i), '\0' });
        workspaces[i].layout_idx = 0;
        workspaces[i].split_ratio = cfg.split_ratio;
        workspaces[i].nmaster = cfg.nmaster;
        workspaces[i].scroll_off = 0;
    }
}

void
workspaces_shutdown(void)
{
    for (unsigned i = 0; i < WS_MAX; i++) {
        free(workspaces[i].name);
        workspaces[i].name = NULL;
        workspaces[i].sel = NULL;
        workspaces[i].mon = NULL;
    }
}

/* dwm-style hide: hidden windows stay MAPPED, parked just off-screen
 * (dwm moves them to WIDTH,HEIGHT+2bw). No unmap events means no
 * withdraw ambiguity and clients like kitty keep their rendering
 * state, repainting the moment they come back. */
void
client_park(wm_t *wm, client_t *c, bool hide)
{
    monitor_t *m = c->ws < WS_MAX ? workspaces[c->ws].mon : NULL;

    if (!m)
        m = focused_mon(wm);
    if (!m)
        return;
    int px = c->x, py = c->y;

    if (hide) {
        px = m->geom.x + (int)m->geom.w + 64;
        py = m->geom.y + (int)m->geom.h + 64;
    }
    uint32_t vals[2] = { (uint32_t)px, (uint32_t)py };

    xcb_configure_window(wm->conn, c->win,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
}

static void
set_ws_members_mapped(wm_t *wm, unsigned idx, bool mapped)
{
    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->scratchpad || c->swallowed_by)
            continue;
        client_park(wm, c, !mapped);
    }
}

static client_t *
ws_focus_candidate(wm_t *wm, workspace_t *ws)
{
    unsigned idx = (unsigned)(ws - workspaces);

    if (ws->sel && ws->sel->ws == idx && !ws->sel->scratch_hidden)
        return ws->sel;
    for (client_t *c = wm->clients; c; c = c->next)
        if (c->ws == idx && !c->scratch_hidden)
            return c;
    return NULL;
}

void
view_ws(wm_t *wm, unsigned idx)
{
    if (idx >= WS_MAX)
        return;
    monitor_t *m = workspaces[idx].mon;
    /* Viewing a workspace homed on another monitor moves focus there
     * (SPEC §4.2 focus-follows-switching). */
    if (idx == m->ws_visible) {
        if (m->ws_prev >= WS_MAX || m->ws_prev == m->ws_visible ||
            workspaces[m->ws_prev].mon != m)
            return;
        idx = m->ws_prev;
    }

    unsigned old = m->ws_visible;
    m->ws_prev = old;
    m->ws_visible = idx;

    /* A designated scratchpad stays mapped over whatever is visible. */
    set_ws_members_mapped(wm, old, false);
    set_ws_members_mapped(wm, idx, true);
    arrange(wm);
    ewmh_update_current_desktop(wm);

    wm->focus_mon = m;
    client_t *c = ws_focus_candidate(wm, &workspaces[idx]);
    if (c)
        focus(wm, c);
    else
        focus_clear(wm);
}

void
send_client_to_ws(wm_t *wm, client_t *c, unsigned idx)
{
    if (!c || idx >= WS_MAX || idx == c->ws)
        return;

    unsigned old = c->ws;
    bool focused_here = wm->focused == c;
    bool to_visible = ws_shown(idx);

    if (workspaces[old].sel == c)
        workspaces[old].sel = NULL;
    c->ws = idx;
    workspaces[idx].sel = c;
    ewmh_update_wm_desktop(wm, c);

    if (!to_visible) {
        if (!c->scratchpad && !c->scratch_hidden)
            client_park(wm, c, true);
    } else if (!c->scratchpad && !c->scratch_hidden) {
        /* Arrivals from a hidden workspace must rejoin the view on
         * screen, or the follow-up SetInputFocus hits a Match error. */
        client_park(wm, c, false);
    }
    arrange(wm);
    if (focused_here) {
        if (to_visible && !c->scratch_hidden)
            focus(wm, c);
        else
            refocus_ws(wm, workspaces[old].mon->ws_visible);
    }
}

void
ws_migrate_focused_to_next(wm_t *wm)
{
    monitor_t *m = focused_mon(wm);
    monitor_t *t;
    unsigned a;
    unsigned b;
    client_t *fc;

    if (!m)
        return;
    t = m->next;
    if (!t)
        return;
    a = m->ws_visible;
    b = t->ws_visible;
    if (a == b)
        return;
    /* Swap the visible pair: each neighbor keeps exactly one shown. */
    m->ws_visible = b;
    m->ws_prev = a;
    t->ws_visible = a;
    t->ws_prev = b;
    set_ws_members_mapped(wm, a, false);
    set_ws_members_mapped(wm, b, false);
    set_ws_members_mapped(wm, b, true);
    set_ws_members_mapped(wm, a, true);
    arrange(wm);
    ewmh_update_current_desktop(wm);
    fc = ws_focus_candidate(wm, &workspaces[m->ws_visible]);
    if (fc)
        focus(wm, fc);
    else
        focus_clear(wm);
}

void
send_focused_to_ws(wm_t *wm, unsigned idx)
{
    send_client_to_ws(wm, wm->focused, idx);
}

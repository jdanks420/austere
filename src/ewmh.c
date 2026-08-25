#define _GNU_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "monitor.h"
#include "util.h"
#include "workspace.h"

#define EWMH_DESKTOP_ALL 0xFFFFFFFFu

void
ewmh_init(wm_t *wm)
{
    atoms_t *a = wm->atoms;
    xcb_atom_t supported[] = {
        a->net_supporting_wm_check,
        a->net_wm_name,
        a->net_client_list,
        a->net_active_window,
        a->net_number_of_desktops,
        a->net_desktop_names,
        a->net_current_desktop,
        a->net_wm_desktop,
        a->net_wm_state,
        a->net_wm_state_demands_attention,
    };
    uint32_t ndesktops = WS_MAX;
    char names[WS_MAX * 64];
    int off = 0;

    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        a->net_supported, XCB_ATOM_ATOM, 32,
        (uint32_t)(sizeof(supported) / sizeof(supported[0])), supported);

    /* Wipe any predecessor's state so clients never observe stale ids
     * between WM generations. */
    xcb_delete_property(wm->conn, wm->scr->root, a->net_client_list);
    xcb_delete_property(wm->conn, wm->scr->root, a->net_active_window);
    xcb_delete_property(wm->conn, wm->scr->root, a->net_current_desktop);
    xcb_delete_property(wm->conn, wm->scr->root, a->net_desktop_names);

    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        a->net_number_of_desktops, XCB_ATOM_CARDINAL, 32, 1, &ndesktops);

    memset(names, 0, sizeof(names));
    for (unsigned i = 0; i < WS_MAX && off < (int)sizeof(names) - 2; i++) {
        size_t len = strlen(workspaces[i].name);

        if (len > sizeof(names) - (size_t)off - 2)
            break;
        memcpy(names + off, workspaces[i].name, len);
        off += (int)len + 1;
    }
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        a->net_desktop_names, a->utf8_string, 8, (uint32_t)off, names);

    ewmh_update_current_desktop(wm);
}

void
ewmh_update_client_list(wm_t *wm)
{
    atoms_t *a = wm->atoms;
    xcb_window_t *ids = NULL;
    unsigned n = 0;

    if (wm->nclients) {
        ids = xmalloc(wm->nclients * sizeof(*ids));
        for (client_t *c = wm->clients; c; c = c->next)
            ids[n++] = c->win;
        /* _NET_CLIENT_LIST is oldest-first (EWMH); our list is newest-first */
        for (unsigned i = 0; i < n / 2; i++) {
            xcb_window_t t = ids[i];
            ids[i] = ids[n - 1 - i];
            ids[n - 1 - i] = t;
        }
    }
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        a->net_client_list, XCB_ATOM_WINDOW, 32, n, ids);
    free(ids);
}

void
ewmh_update_wm_desktop(wm_t *wm, client_t *c)
{
    uint32_t d = c->ws;

    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, c->win,
        wm->atoms->net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &d);
}

void
ewmh_update_current_desktop(wm_t *wm)
{
    uint32_t d = wm->mons ? wm->mons->ws_visible : 0;

    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        wm->atoms->net_current_desktop, XCB_ATOM_CARDINAL, 32, 1, &d);
}

static bool
state_has(xcb_connection_t *conn, xcb_atom_t state_atom,
    xcb_window_t win, xcb_atom_t wanted, uint32_t *n_out)
{
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(conn,
            xcb_get_property(conn, 0, win, state_atom, XCB_ATOM_ATOM, 0, 64),
            NULL);
    bool found = false;
    uint32_t n = 0;

    if (!r)
        return false;
    xcb_atom_t *atoms = xcb_get_property_value(r);
    n = r->value_len;
    for (uint32_t i = 0; i < n; i++)
        if (atoms[i] == wanted)
            found = true;
    free(r);
    if (n_out)
        *n_out = n;
    return found;
}

void
ewmh_set_demands_attention(wm_t *wm, client_t *c, bool on)
{
    atoms_t *a = wm->atoms;
    bool has;
    uint32_t n;

    has = state_has(wm->conn, a->net_wm_state, c->win,
        a->net_wm_state_demands_attention, &n);
    if (on == has)
        return;

    if (on) {
        xcb_change_property(wm->conn, XCB_PROP_MODE_APPEND, c->win,
            a->net_wm_state, XCB_ATOM_ATOM, 32, 1,
            &a->net_wm_state_demands_attention);
    } else if (n > 0) {
        xcb_atom_t kept[64];
        xcb_get_property_reply_t *r =
            xcb_get_property_reply(wm->conn,
                xcb_get_property(wm->conn, 0, c->win, a->net_wm_state,
                    XCB_ATOM_ATOM, 0, 64), NULL);
        uint32_t m = 0;

        if (!r)
            return;
        xcb_atom_t *atoms = xcb_get_property_value(r);
        for (uint32_t i = 0; i < r->value_len; i++)
            if (atoms[i] != a->net_wm_state_demands_attention &&
                m < sizeof(kept) / sizeof(kept[0]))
                kept[m++] = atoms[i];
        free(r);
        xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, c->win,
            a->net_wm_state, XCB_ATOM_ATOM, 32, m, kept);
    }
}

void
ewmh_update_workarea(wm_t *wm)
{
    monitor_t *m = focused_mon(wm);

    if (!m)
        return;
    Rect a = mon_workarea(m);

    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->scr->root,
        wm->atoms->net_workarea, XCB_ATOM_CARDINAL, 32, 4,
        (uint32_t[]){ (uint32_t)a.x, (uint32_t)a.y, a.w, a.h });
}

void
ewmh_set_fullscreen(wm_t *wm, client_t *c, bool on)
{
    atoms_t *a = wm->atoms;

    if (on) {
        xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, c->win,
            a->net_wm_state, XCB_ATOM_ATOM, 32, 1,
            &a->net_wm_state_fullscreen);
    } else {
        xcb_atom_t kept[8];
        unsigned m = 0;

        xcb_get_property_reply_t *r = xcb_get_property_reply(wm->conn,
            xcb_get_property(wm->conn, 0, c->win, a->net_wm_state,
                XCB_ATOM_ATOM, 0, 32),
            NULL);
        if (r) {
            xcb_atom_t *atoms =
                xcb_get_property_value(r);
            for (uint32_t i = 0; i < r->value_len; i++)
                if (atoms[i] != a->net_wm_state_fullscreen &&
                    m < 8)
                    kept[m++] = atoms[i];
            free(r);
        }
        xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, c->win,
            a->net_wm_state, XCB_ATOM_ATOM, 32, m, kept);
    }
}

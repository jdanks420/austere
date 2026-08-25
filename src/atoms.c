#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atoms.h"

int
atoms_init(atoms_t *a, xcb_connection_t *conn, int scr_index)
{
    char wm_sn[16];
    snprintf(wm_sn, sizeof(wm_sn), "WM_S%d", scr_index);

    const char *names[] = {
        wm_sn,
        "MANAGER",
        "UTF8_STRING",
        "_NET_SUPPORTING_WM_CHECK",
        "_NET_WM_NAME",
        "WM_PROTOCOLS",
        "WM_DELETE_WINDOW",
        "_NET_SUPPORTED",
        "_NET_CLIENT_LIST",
        "_NET_ACTIVE_WINDOW",
        "WM_HINTS",
        "WM_STATE",
        "_NET_NUMBER_OF_DESKTOPS",
        "_NET_DESKTOP_NAMES",
        "_NET_CURRENT_DESKTOP",
        "_NET_WM_DESKTOP",
        "_NET_WM_STATE",
        "_NET_WM_STATE_DEMANDS_ATTENTION",
        "_NET_WM_STATE_FULLSCREEN",
        "_NET_CLOSE_WINDOW",
        "_NET_WM_PID",
        "_NET_WORKAREA",
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK",
        "_AUSTERE_TEST_MONITORS",
    };
    enum { N = (int)(sizeof(names) / sizeof(names[0])) };
    xcb_intern_atom_cookie_t cookies[N];
    xcb_intern_atom_reply_t *r;
    xcb_atom_t *slots[N] = {
        &a->wm_sn,
        &a->manager,
        &a->utf8_string,
        &a->net_supporting_wm_check,
        &a->net_wm_name,
        &a->wm_protocols,
        &a->wm_delete_window,
        &a->net_supported,
        &a->net_client_list,
        &a->net_active_window,
        &a->wm_hints,
        &a->wm_state,
        &a->net_number_of_desktops,
        &a->net_desktop_names,
        &a->net_current_desktop,
        &a->net_wm_desktop,
        &a->net_wm_state,
        &a->net_wm_state_demands_attention,
        &a->net_wm_state_fullscreen,
        &a->net_close_window,
        &a->net_wm_pid,
        &a->net_workarea,
        &a->net_wm_window_type,
        &a->net_wm_window_type_dock,
        &a->austere_test_monitors,
    };

    for (int i = 0; i < N; i++)
        cookies[i] =
            xcb_intern_atom(conn, 0, (uint16_t)strlen(names[i]), names[i]);
    for (int i = 0; i < N; i++) {
        if (!(r = xcb_intern_atom_reply(conn, cookies[i], NULL)))
            return -1;
        *slots[i] = r->atom;
        free(r);
    }
    return 0;
}

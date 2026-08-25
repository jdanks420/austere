#ifndef AUSTERE_ATOMS_H
#define AUSTERE_ATOMS_H

#include <xcb/xcb.h>

struct atoms {
    xcb_atom_t wm_sn;
    xcb_atom_t manager;
    xcb_atom_t utf8_string;
    xcb_atom_t net_supporting_wm_check;
    xcb_atom_t net_wm_name;
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_atom_t net_supported;
    xcb_atom_t net_client_list;
    xcb_atom_t net_active_window;
    xcb_atom_t wm_hints;
    xcb_atom_t wm_state;
    xcb_atom_t net_number_of_desktops;
    xcb_atom_t net_desktop_names;
    xcb_atom_t net_current_desktop;
    xcb_atom_t net_wm_desktop;
    xcb_atom_t net_wm_state;
    xcb_atom_t net_wm_state_demands_attention;
    xcb_atom_t net_wm_state_fullscreen;
    xcb_atom_t net_close_window;
    xcb_atom_t net_wm_pid;
    xcb_atom_t net_workarea;
    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t austere_test_monitors;
};

typedef struct atoms atoms_t;

int atoms_init(atoms_t *a, xcb_connection_t *conn, int scr_index);

#endif

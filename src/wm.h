#ifndef AUSTERE_WM_H
#define AUSTERE_WM_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "atoms.h"
#include "monitor.h"

struct client;
typedef struct client client_t;
struct wm;
typedef struct wm wm_t;
struct font;
typedef struct font font_t;

#include "mouse.h"

typedef struct wm {
    const char *self;
    char **argv; /* saved for execvp restart-in-place */
    bool replace;
    bool restarted; /* re-exec'd by wm_restart: skip autostart */
    xcb_connection_t *conn;
    xcb_screen_t *scr;
    int scr_index;
    atoms_t *atoms;
    xcb_key_symbols_t *keysyms;
    xcb_window_t mgr_win;
    int pipe[2];
    volatile sig_atomic_t running;

    monitor_t *mons; /* RandR output list (SPEC §4.3) */
    monitor_t *focus_mon; /* last monitor with input focus */
    int randr_event_base; /* -1 when the extension is absent */
    client_t *clients; /* newest first */
    client_t *focused;
    unsigned nclients;
    mouse_t mouse;
    font_t *fonts; /* core-font cache owned by draw.c */
} wm_t;

int wm_main(int argc, char **argv);

#endif

#ifndef AUSTERE_WM_H
#define AUSTERE_WM_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "atoms.h"
#include "monitor.h"
#include "mouse.h"

struct client;
typedef struct client client_t;
struct wm;
typedef struct wm wm_t;
struct font;
typedef struct font font_t;

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
    client_t *mru;     /* most-recently-focused first (switcher) */
    client_t *focused;
    unsigned nclients;
    mouse_t mouse;
    font_t *fonts; /* fontconfig-pattern cache owned by draw.c */
    void *ftlib; /* FT_Library handle, owned by draw.c */
    uint32_t *textbuf; /* scratch ARGB line buffer, owned by draw.c */
    unsigned textbuf_cap;
    unsigned layout_idx; /* active layout for ALL workspaces (SPEC §4.2) */
    bool deco_argb; /* compositor present + 32-bit visual usable */
    uint8_t deco_argb_depth;
    xcb_visualid_t deco_argb_visual;
} wm_t;

int wm_main(int argc, char **argv);
void wm_restart(wm_t *wm);

#endif

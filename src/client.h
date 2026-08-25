#ifndef AUSTERE_CLIENT_H
#define AUSTERE_CLIENT_H

#include <stdbool.h>
#include <xcb/xcb.h>
#include "wm.h"

struct client {
    xcb_window_t win;
    unsigned ws; /* owning workspace index in workspaces[] */
    client_t *next, *prev;

    char *name;
    char *cls;          /* WM_CLASS class part (ICCCM) */ /* WM_NAME copy; owned by client */
    bool floating;
    bool ever_mapped;
    bool scratchpad;    /* designated dropdown window (SPEC §5.5) */
    bool fullscreen;    /* _NET_WM_STATE_FULLSCREEN (EWMH) */
    pid_t pid;          /* _NET_WM_PID, read at manage */
    int base_w, base_h;     /* ICCCM base size for increment clamping */
    int inc_w, inc_h;       /* resize increments (0 = none) */
    struct client *swallow_victim;  /* hidden terminal this client replaced */
    struct client *swallowed_by;    /* set on the hidden victim */
    bool scratch_hidden;
    bool urgent;

    int x, y;
    unsigned w, h;
    unsigned min_w, min_h;
};

void manage(wm_t *wm, xcb_window_t win);
void unmanage(wm_t *wm, xcb_window_t win);
client_t *find_client(wm_t *wm, xcb_window_t win);
void apply_geom(wm_t *wm, client_t *c, int x, int y, unsigned w, unsigned h);
void focus(wm_t *wm, client_t *c);
void focus_clear(wm_t *wm);
void refocus_ws(wm_t *wm, unsigned idx);
void client_set_urgent(wm_t *wm, client_t *c, bool urgent);
void client_poll_urgency(wm_t *wm, client_t *c);
void client_refresh_name(wm_t *wm, client_t *c);
void set_border(wm_t *wm, client_t *c, unsigned long color);
void client_shape(wm_t *wm, client_t *c, unsigned radius);
void ws_recompute_urgent(wm_t *wm, unsigned idx);

#endif

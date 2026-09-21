#ifndef AUSTERE_DECO_H
#define AUSTERE_DECO_H

#include <stdbool.h>
#include <xcb/xcb.h>

#include "wm.h"
#include "client.h"
#include "draw.h"

typedef struct deco {
    xcb_window_t win;
    xcb_colormap_t cmap;
    unsigned title_h;
    int win_x, win_y;
    unsigned win_w, win_h;
    int orig_x, orig_y;
    unsigned orig_w, orig_h;
    bool maximized;
    bool prev_floating;
    bool transparent;
    int close_x, close_y;
    int max_x, max_y;
    int min_x, min_y;
    unsigned btn_size;
    draw_t draw;
} deco_t;

void deco_create(wm_t *wm, client_t *c);
void deco_init(wm_t *wm);
void deco_destroy(wm_t *wm, client_t *c);
void deco_cleanup(wm_t *wm, deco_t *d);
void deco_update(wm_t *wm, client_t *c);
void deco_draw(wm_t *wm, client_t *c);
void deco_shape(wm_t *wm, client_t *c);
void deco_reconfigure_all(wm_t *wm);
void deco_toggle_maximize(wm_t *wm, client_t *c);
void deco_minimize(wm_t *wm, client_t *c);
void deco_restore_minimized(wm_t *wm);
bool deco_button_hit(wm_t *wm, xcb_button_press_event_t *ev,
    unsigned btn);
client_t *find_client_by_deco(wm_t *wm, xcb_window_t win);

#endif

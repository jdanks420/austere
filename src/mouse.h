#ifndef AUSTERE_MOUSE_H
#define AUSTERE_MOUSE_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

typedef enum {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE,
    DRAG_TILE_RESIZE, /* tiling-boundary drag: alters ws->split_ratio */
} drag_mode_t;

typedef struct mouse {
    drag_mode_t mode;
    struct client *drag;
    int press_x, press_y;   /* pointer at press, root coords */
    int orig_x, orig_y;     /* client geometry at press */
    unsigned orig_w, orig_h;
    double tile_ratio;      /* ws->split_ratio at press (TILE_RESIZE) */
    int tile_scale;         /* px width the ratio maps across */
} mouse_t;

void mouse_grab_client(struct wm *wm, xcb_window_t win);
void mouse_press(struct wm *wm, xcb_button_press_event_t *ev);
void mouse_motion(struct wm *wm, xcb_motion_notify_event_t *ev);
void mouse_release(struct wm *wm, xcb_button_release_event_t *ev);
void move_drag_begin(struct wm *wm, struct client *c,
    xcb_button_press_event_t *ev);

#endif

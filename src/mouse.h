#ifndef AUSTERE_MOUSE_H
#define AUSTERE_MOUSE_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

/* client_t/wm_t are completed by wm.h, which includes this header after
 * forward-declaring them; pointer use here is legal either way. */

typedef enum {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE,
} drag_mode_t;

typedef struct mouse {
    drag_mode_t mode;
    struct client *drag;
    int press_x, press_y;   /* pointer at press, root coords */
    int orig_x, orig_y;     /* client geometry at press */
    unsigned orig_w, orig_h;
} mouse_t;

void mouse_grab_client(struct wm *wm, xcb_window_t win);
void mouse_press(struct wm *wm, xcb_button_press_event_t *ev);
void mouse_motion(struct wm *wm, xcb_motion_notify_event_t *ev);
void mouse_release(struct wm *wm, xcb_button_release_event_t *ev);

#endif

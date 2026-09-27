#ifndef AUSTERE_TRAY_H
#define AUSTERE_TRAY_H

#include <stdbool.h>

#include "draw.h"
#include "wm.h"

/* StatusNotifierItem tray (SPEC §7.6). This header is D-Bus free and
 * compiles unchanged in every build variant, including AUSTERE_NO_DBUS=1,
 * where the backend is an inert no-op that reports an empty view.
 *
 * The backend owns every string, image and item identity below. UI only
 * borrows the pointers handed out by tray_view() until the next
 * tray_pump(), tray_tick(), tray_set_icon_size() or tray_shutdown():
 * readers allocate nothing, free nothing and never trigger bus I/O.
 * Action indices are snapshot-relative and are resolved to a stable item
 * identity inside the backend call. */

#define TRAY_MAX_ITEMS 32
#define TRAY_TITLE_MAX 128

typedef enum {
    TRAY_STATUS_PASSIVE = 0,  /* hidden from the view, per the spec */
    TRAY_STATUS_ACTIVE,
    TRAY_STATUS_ATTENTION,
} tray_status_t;

typedef struct {
    char title[TRAY_TITLE_MAX]; /* NewTitle, "" when the item has none */
    tray_status_t status;       /* normalized, never a raw spec string */
    const char *icon_name;      /* effective name, "" when unresolved */
    const image_t *img;         /* backend-owned pixels, NULL if none */
    const char *menu_path;      /* com.canonical.dbusmenu path, "" if none */
} tray_item_t;

typedef struct {
    const tray_item_t *items;
    unsigned nitems;
} tray_view_t;

void tray_init(wm_t *wm);
void tray_shutdown(wm_t *wm);

/* File descriptor to poll, or -1 when there is no live connection. */
int tray_fd(void);

/* Drain and dispatch pending D-Bus traffic (call on POLLIN). */
void tray_pump(wm_t *wm);

/* Milliseconds until the next internal deadline (death grace, stalled
 * property fetch), or -1 when nothing is pending. */
int tray_timeout_ms(wm_t *wm);

/* Fire the deadlines tray_timeout_ms() promised. */
void tray_tick(wm_t *wm);

bool tray_available(void);

/* Icon decode size in pixels; re-decodes only when the value changes. */
void tray_set_icon_size(wm_t *wm, unsigned px);

/* True when the visible snapshot changed since this was last asked, and
 * clears the flag: the caller owns the repaint. The backend has no bar
 * or drawing dependency, so the event loop turns this into at most one
 * bar_render_all() of its own. */
bool tray_render_pending(void);

tray_view_t tray_view(void);

void tray_click(unsigned idx, unsigned btn, int root_x, int root_y);
void tray_scroll(unsigned idx, int delta, bool horizontal);

#endif

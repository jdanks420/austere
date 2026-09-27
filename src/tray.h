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

/* Bounds of the flat menu view. A menu that offers more rows than this
 * is published truncated, and one whose label does not fit is cut at
 * the last byte that does, so a hostile client cannot make the popup
 * allocate from a string it chose. */
#define TRAY_MENU_MAX_ROWS 32
#define TRAY_MENU_LABEL_MAX 128

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
    bool is_menu;               /* ItemIsMenu: the icon is the whole menu */
} tray_item_t;

typedef struct {
    const tray_item_t *items;
    unsigned nitems;
} tray_view_t;

/* Flat DBusMenu view, published by the backend and drawn by the popup.
 * The backend owns every row, label, image and the title; the popup
 * allocates nothing, frees nothing and never touches the bus.
 *
 * Lifetime: rows, label strings and the title are borrowed until the
 * next call that can publish, i.e. the next tray_menu_open(),
 * tray_menu_close(), tray_menu_click(), tray_pump() or tray_tick().
 * A reader that needs them past such a call re-reads tray_menu_view()
 * and compares epoch: the epoch changes exactly when the published
 * content changes, so an unchanged epoch means the snapshot it holds is
 * still the current one. Compare it for inequality only, never for
 * magnitude, so wrap-around stays harmless. */
typedef enum {
    TRAY_MENU_HIDDEN = 0,  /* no menu: the popup stays closed */
    TRAY_MENU_LOADING,     /* AboutToShow/GetLayout in flight */
    TRAY_MENU_READY,       /* rows are the menu's own content */
    TRAY_MENU_FAILED,      /* no menu, or the exchange failed */
} tray_menu_state_t;

typedef enum {
    TRAY_MENU_ROW_NORMAL = 0,
    TRAY_MENU_ROW_SEPARATOR,  /* no label, no icon, never clickable */
    TRAY_MENU_ROW_TOGGLE,     /* checked is the box state */
    TRAY_MENU_ROW_RADIO,      /* checked is the dot state */
} tray_menu_rowtype_t;

typedef struct {
    tray_menu_rowtype_t type;
    bool checked;             /* toggle and radio only, false elsewhere */
    bool enabled;             /* a disabled row is drawn, not clickable */
    const char *label;        /* backend-owned, "" for a separator */
    const image_t *img;       /* backend-owned, NULL when the row has none */
    unsigned id;              /* the wire id, echoed back by the click */
} tray_menu_row_t;

typedef struct {
    tray_menu_state_t state;
    unsigned epoch;           /* bumped on every published change */
    const tray_menu_row_t *rows;
    unsigned nrows;            /* at most TRAY_MENU_MAX_ROWS */
    int anchor_x, anchor_y;   /* the press point, in root coordinates */
    const char *title;        /* backend-owned, "" when the menu has none */
} tray_menu_view_t;

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

/* Open the item's own menu, anchored at a root-space point. Never
 * blocks: the DBusMenu exchange is asynchronous and the view moves
 * HIDDEN -> LOADING -> READY, or to FAILED when the item has no usable
 * internal menu, in which case the backend sends the item's ContextMenu
 * instead. Returns false, changing nothing, when the index names no
 * visible item or there is no live bus, so the caller can fall back to
 * the item's ContextMenu itself. */
bool tray_menu_open(unsigned idx, int root_x, int root_y);

/* Drop the current menu: dismissal, the item dying, or the bus going
 * away. Idempotent, and never a bus call of its own. */
void tray_menu_close(void);

/* Activate a row of the current snapshot by index, like tray_click()
 * the index is snapshot-relative and is resolved to the row's wire id
 * inside the backend call. A row that is out of range, a separator, a
 * disabled row, or any state other than READY, is a no-op. */
void tray_menu_click(unsigned row);

tray_menu_view_t tray_menu_view(void);

#endif

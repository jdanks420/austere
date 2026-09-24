#ifndef AUSTERE_POPUP_H
#define AUSTERE_POPUP_H

#include "wm.h"

/* Toasts (SPEC §7.5): a per-monitor stack in the work area's top-right
 * corner carrying desktop notifications (D-Bus, notify.c) and the WM's
 * own status messages. Oldest on top, newest at the bottom; each one
 * auto-expires unless the sender marked it resident. Clicking a toast
 * dismisses it, clicking an action invokes it. */

#define TOAST_MAX 4
#define TOAST_URGENCY_NORMAL 0
#define TOAST_URGENCY_CRITICAL 2

typedef struct {
    char *key;   /* action id sent back over D-Bus ("default" etc.) */
    char *label; /* text drawn in the toast */
} toast_action_t;

/* Reason codes per the notification spec. */
#define TOAST_CLOSED_EXPIRED 1
#define TOAST_CLOSED_DISMISSED 2
#define TOAST_CLOSED_PROGRAMMATIC 3

/* Notified when a toast goes away for any reason, so the D-Bus layer
 * can emit NotificationClosed. */
typedef void (*toast_closed_fn)(unsigned id, unsigned reason, void *ud);

/* Notified when the user activates a toast action, so the D-Bus layer
 * can emit ActionInvoked. */
typedef void (*toast_action_fn)(unsigned id, const char *key, void *ud);

void popups_init(wm_t *wm);
void popups_sync(wm_t *wm); /* topology changed: drop and rebuild windows */
void popups_shutdown(wm_t *wm);

/* WM status message ("config reloaded", "launch: no match"). Short
 * lived, no icon, no actions, also mirrored to stderr. */
void popup_notify(wm_t *wm, const char *fmt, ...);

/* Desktop notification from the D-Bus service. Returns the toast id. */
unsigned toast_add(wm_t *wm, const char *app, const char *summary,
    const char *body, const char *icon, int urgency, int timeout_ms,
    unsigned replaces_id, bool resident, const toast_action_t *actions,
    unsigned nactions);

/* Close by id (CloseNotification). Unknown ids are ignored, as the
 * spec requires. */
void toast_close(wm_t *wm, unsigned id, unsigned reason);

void popups_set_closed_cb(toast_closed_fn fn, void *ud);
void popups_set_action_cb(toast_action_fn fn, void *ud);

/* Milliseconds until the next expiry, -1 when nothing is showing. */
int popups_timeout_ms(wm_t *wm);
void popups_tick(wm_t *wm);

/* Click routing for the toast windows; true when consumed. */
bool popup_button(wm_t *wm, xcb_window_t win, int x, int y, uint8_t btn);

/* Repaint a toast window after an expose; true when the window was one
 * of ours. */
bool popup_expose(wm_t *wm, xcb_window_t win);

#endif

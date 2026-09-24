#ifndef AUSTERE_NOTIFY_H
#define AUSTERE_NOTIFY_H

#include "wm.h"

/* Desktop notification service (org.freedesktop.Notifications).
 * austere owns the bus name and renders what it receives through the
 * toast stack (popup.c). Compiled out entirely with AUSTERE_NO_DBUS=1,
 * in which case the WM simply never owns the name. */

void notify_init(wm_t *wm);
void notify_shutdown(wm_t *wm);

/* File descriptor to poll, or -1 when the service is not running. */
int notify_fd(void);

/* Drain and dispatch pending D-Bus messages (call on POLLIN). */
void notify_pump(wm_t *wm);

#endif

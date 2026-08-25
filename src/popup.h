#ifndef AUSTERE_POPUP_H
#define AUSTERE_POPUP_H

#include "wm.h"

/* Internal notifications (SPEC §7.5): one region per monitor, FIFO
 * depth 3, auto-dismiss after popup_timeout, pass-through input,
 * always mirrored to stderr. */

#define POPUP_DEPTH 3

void popups_init(wm_t *wm);
void popups_sync(wm_t *wm); /* topology changed: create/resize/destroy */
void popup_notify(wm_t *wm, const char *fmt, ...);
int popups_timeout_ms(wm_t *wm); /* -1 when nothing is showing */
void popups_tick(wm_t *wm);      /* dismiss expired, redraw */
void popups_shutdown(wm_t *wm);

#endif

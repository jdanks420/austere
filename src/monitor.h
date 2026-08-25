#ifndef AUSTERE_MONITOR_H
#define AUSTERE_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

struct wm;
typedef struct wm wm_t;

typedef struct {
    int x, y;
    unsigned w, h;
} Rect;

/* SPEC §4.3: a RandR output region showing exactly one workspace. The
 * list is owned by wm_t; workspaces point into it. */
struct bar; /* bar.h; heap-allocated to avoid a header cycle */

struct monitor {
    Rect geom;
    unsigned ws_visible;
    unsigned ws_prev;
    struct bar *bar; /* SPEC §4.3 embeds by value; pointer breaks the
                        monitor.h ↔ bar.h cycle */
    struct monitor *next;
};

typedef struct monitor monitor_t;

monitor_t *monitors_init(wm_t *wm);
void monitors_shutdown(wm_t *wm);
void monitors_apply(wm_t *wm, const Rect *rects, unsigned n);
void monitors_refresh(wm_t *wm);
monitor_t *focused_mon(wm_t *wm);
bool ws_shown(unsigned idx);

#endif

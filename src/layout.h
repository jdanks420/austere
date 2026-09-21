#ifndef AUSTERE_LAYOUT_H
#define AUSTERE_LAYOUT_H

#include "wm.h"
#include "workspace.h"

/* Layout contract (SPEC §4.4): read WM state, write geometry exclusively
 * through apply_geom(), tolerate 0/1 clients, never touch
 * xcb_configure_window directly. Floating clients are invisible to
 * tiling layouts. */
typedef struct layout {
    const char *name;
    const char *symbol; /* bar glyph, e.g. "[]=" (SPEC §4.4) */
    void (*arrange)(wm_t *wm, monitor_t *mon, workspace_t *ws);
    bool floats_all;    /* true => arrange() leaves every client alone */
    bool ratio_aware;   /* true => use ws->split_ratio (tile resize) */
} layout_t;

extern const layout_t austere_layouts[];
extern const unsigned n_austere_layouts;

void tile_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws);
void monocle_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws);
void float_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws);

void arrange(wm_t *wm);
const layout_t *layout_by_name(const char *name);
void set_layout(wm_t *wm, const char *name);
void cycle_layout(wm_t *wm);
void adjust_mwfact(wm_t *wm, double delta);
void adjust_nmaster(wm_t *wm, int delta);
void toggle_float(wm_t *wm);

#endif

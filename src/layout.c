#include <stdio.h>
#include <string.h>

#include "bar.h"
#include "client.h"
#include "layout.h"
#include "settings.h"
#include "monitor.h"
#include "util.h"

const layout_t austere_layouts[] = {
    { "tile", "[]=", tile_arrange },
    { "monocle", "[ ]", monocle_arrange },
    { "scroll", "><", scroll_arrange },
};

const unsigned n_austere_layouts =
    sizeof(austere_layouts) / sizeof(austere_layouts[0]);

static workspace_t *
visible_ws(wm_t *wm)
{
    monitor_t *m = focused_mon(wm);

    return &workspaces[m ? m->ws_visible : 0];
}

void
arrange(wm_t *wm)
{
    for (monitor_t *m = wm->mons; m; m = m->next) {
        workspace_t *ws = &workspaces[m->ws_visible];

        austere_layouts[ws->layout_idx].arrange(wm, m, ws);
    }

    /* Floaters must sit above tiled geometry so newly mapped clients
     * don't bury them. */
    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->floating && !c->scratch_hidden &&
            workspaces[c->ws].mon &&
            workspaces[c->ws].mon->ws_visible == c->ws)
            xcb_configure_window(wm->conn, c->win,
                XCB_CONFIG_WINDOW_STACK_MODE,
                (uint32_t[]){ XCB_STACK_MODE_ABOVE });
    }
}

const layout_t *
layout_by_name(const char *name)
{
    for (unsigned i = 0; i < n_austere_layouts; i++)
        if (!strcmp(austere_layouts[i].name, name))
            return &austere_layouts[i];
    return NULL;
}

void
set_layout(wm_t *wm, const char *name)
{
    const layout_t *l = layout_by_name(name);

    if (!l) {
        fprintf(stderr, "austere: unknown layout '%s'\n", name);
        return;
    }
    visible_ws(wm)->layout_idx = (unsigned)(l - austere_layouts);
    arrange(wm);
}

void
cycle_layout(wm_t *wm)
{
    workspace_t *ws = visible_ws(wm);

    ws->layout_idx = (ws->layout_idx + 1) % n_austere_layouts;
    arrange(wm);
}

void
adjust_mwfact(wm_t *wm, double delta)
{
    workspace_t *ws = visible_ws(wm);

    ws->split_ratio += delta;
    if (ws->split_ratio < 0.1)
        ws->split_ratio = 0.1;
    if (ws->split_ratio > 0.9)
        ws->split_ratio = 0.9;
    arrange(wm);
}

void
adjust_nmaster(wm_t *wm, int delta)
{
    workspace_t *ws = visible_ws(wm);
    int n = (int)ws->nmaster + delta;

    if (n < 1)
        n = 1;
    if (n > 10)
        n = 10;
    ws->nmaster = (unsigned)n;
    arrange(wm);
}

void
toggle_float(wm_t *wm)
{
    client_t *c = wm->focused;

    if (!c)
        return;
    c->floating = !c->floating;
    arrange(wm);
}

/* Scroll layout panning (nwm-style): move the viewport by one column.
 * Clamping happens in scroll_arrange against the real column count. */
void
scroll_nudge(wm_t *wm, int dir)
{
    workspace_t *ws = visible_ws(wm);

    if (austere_layouts[ws->layout_idx].arrange != scroll_arrange)
        return;
    monitor_t *m = focused_mon(wm);
    Rect a = mon_workarea(m);
    unsigned gap = cfg.gap;
    unsigned aw = a.w > 2 * gap ? a.w - 2 * gap : a.w;

    ws->scroll_off = (unsigned)((int)ws->scroll_off +
        dir * (int)(aw + gap));
    arrange(wm);
}

void
scroll_left(wm_t *wm)
{
    scroll_nudge(wm, -1);
}

void
scroll_right(wm_t *wm)
{
    scroll_nudge(wm, 1);
}

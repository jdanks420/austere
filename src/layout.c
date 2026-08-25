#include <stdio.h>
#include <string.h>

#include "client.h"
#include "layout.h"
#include "settings.h"
#include "monitor.h"
#include "util.h"

const layout_t austere_layouts[] = {
    { "tile", "[]=", tile_arrange },
    { "monocle", "[ ]", monocle_arrange },
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
}

void
set_layout(wm_t *wm, const char *name)
{
    for (unsigned i = 0; i < n_austere_layouts; i++) {
        if (!strcmp(austere_layouts[i].name, name)) {
            visible_ws(wm)->layout_idx = i;
            arrange(wm);
            return;
        }
    }
    fprintf(stderr, "austere: unknown layout '%s'\n", name);
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

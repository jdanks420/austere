#include <stdio.h>
#include <string.h>

#include "bar.h"
#include "client.h"
#include "layout.h"
#include "settings.h"
#include "monitor.h"
#include "menu.h"
#include "util.h"

const layout_t austere_layouts[] = {
    { "tile", "[]=", tile_arrange, false, true },
    { "monocle", "[ ]", monocle_arrange, false, false },
    { "float", "<>", float_arrange, true, false },
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
    /* Layout mode is global: the same layout applies to every workspace
     * (SPEC §4.2). Parameters like split_ratio/nmaster stay per-ws. */
    const layout_t *lay = &austere_layouts[wm->layout_idx];

    for (monitor_t *m = wm->mons; m; m = m->next) {
        workspace_t *ws = &workspaces[m->ws_visible];

        lay->arrange(wm, m, ws);
    }

    /* Floaters must sit above tiled geometry so newly mapped clients
     * don't bury them. In a float layout there is no tiled geometry, so
     * nothing to protect — focus() owns raising there; re-raising every
     * client on every arrange would force a restack storm (flicker). */
    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->floating && !c->scratch_hidden &&
            workspaces[c->ws].mon &&
            workspaces[c->ws].mon->ws_visible == c->ws)
            raise_client(wm, c);
    }
    if (menu_active())
        menu_bump(wm);
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
    wm->layout_idx = (unsigned)(l - austere_layouts);
    arrange(wm);
    bar_render_all(wm);
}

void
cycle_layout(wm_t *wm)
{
    wm->layout_idx = (wm->layout_idx + 1) % n_austere_layouts;
    arrange(wm);
    bar_render_all(wm);
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



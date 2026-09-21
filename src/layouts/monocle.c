#include "bar.h"
#include "client.h"
#include "layout.h"
#include "settings.h"
#include "workspace.h"

/* Every client fills the workarea; only the focused one is raised into
 * view. Scratchpad members never tile (SPEC §5.5). */
void
monocle_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws)
{
    unsigned idx = (unsigned)(ws - workspaces);
    Rect area = mon_workarea(mon);
    unsigned title_h = cfg.deco ? cfg.deco_title_h : 0;
    unsigned gap = cfg.gap;

    if (cfg.smart_gaps && ws_count_clients(wm, idx) == 1)
        gap = 0;

    int ax = area.x + (int)gap;
    int ay = area.y + (int)gap;
    int aw = (int)area.w - 2 * (int)gap;
    int ah = (int)area.h - 2 * (int)gap;

    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->floating || c->scratchpad ||
            c->swallowed_by || c->minimized)
            continue;
        apply_geom(wm, c, ax, ay + (int)title_h, (unsigned)aw, (unsigned)ah - title_h);
    }
}

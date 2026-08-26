#include "bar.h"
#include "settings.h"
#include "client.h"
#include "layout.h"
#include "workspace.h"

/* Master column on the left at the workspace's split_ratio, remaining
 * clients stacked to the right. Newest client takes the first master
 * slot. Scratchpad members never tile (SPEC §5.5). */
void
tile_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws)
{
    unsigned idx = (unsigned)(ws - workspaces);
    Rect area = mon_workarea(mon);
    unsigned n = 0, nm, i = 0;
    int mw;

    for (client_t *c = wm->clients; c; c = c->next)
        if (c->ws == idx && !c->floating && !c->scratchpad && !c->swallowed_by)
            n++;
    if (n == 0)
        return;

    unsigned gap = cfg.gap;

    if (cfg.smart_gaps && n == 1)
        gap = 0;
    int aw = (int)area.w - 2 * (int)gap;
    int ah = (int)area.h - 2 * (int)gap;
    int ax = area.x + (int)gap;
    int ay = area.y + (int)gap;

    nm = ws->nmaster < n ? ws->nmaster : n;
    mw = (int)((double)aw * ws->split_ratio);

    unsigned mcol = nm < n ? nm : n;
    unsigned scol = n - mcol;
    int mh = ah - (int)((mcol - 1) * gap);
    int sh = ah - (int)((scol - 1) * gap);

    int my = ay;     /* running y in master column */
    int sy = ay;     /* running y in stack column */
    unsigned sp = 0; /* placements made in stack column */

    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->floating || c->scratchpad || c->swallowed_by)
            continue;

        if (i < nm) {
            unsigned mp = i; /* placements made in master column */
            int h = (mp + 1 == mcol) ? ah - my + ay : mh / (int)mcol;
            int w = (mcol == n) ? aw : mw;
            apply_geom(wm, c, ax, my, (unsigned)w, (unsigned)h);
            my += h + (int)gap;
        } else {
            int h = (sp + 1 == scol) ? ah - sy + ay : sh / (int)scol;
            apply_geom(wm, c, ax + mw + (nm < n ? (int)gap : 0), sy,
                (unsigned)(aw - mw - (nm < n ? (int)gap : 0)),
                (unsigned)h);
            sy += h + (int)gap;
            sp++;
        }
        i++;
    }
}

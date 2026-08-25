#include "bar.h"
#include "client.h"
#include "layout.h"
#include "workspace.h"

/* Every client fills the workarea; only the focused one is raised into
 * view. Scratchpad members never tile (SPEC §5.5). */
void
monocle_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws)
{
    unsigned idx = (unsigned)(ws - workspaces);

    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->floating || c->scratchpad || c->swallowed_by)
            continue;
        Rect area = mon_workarea(mon);

        apply_geom(wm, c, area.x, area.y, area.w, area.h);
    }
}

#include "layout.h"
#include "workspace.h"

/* Free positioning: leave every client exactly where it is. Floating is
 * an explicit state entered via toggle_float (super+g); this layout
 * simply never re-tiles anything, so all clients behave as floating
 * (SPEC §4.4 floats_all). */
void
float_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws)
{
    (void)wm;
    (void)mon;
    (void)ws;
}

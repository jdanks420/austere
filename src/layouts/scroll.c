#include "bar.h"
#include "client.h"
#include "layout.h"
#include "settings.h"
#include "workspace.h"

/* Horizontal scroll layout, following niri's model: the workarea is a
 * viewport over an infinite horizontal strip of half-view-width
 * columns. View position (ws->scroll_off) and focus are DECOUPLED —
 * panning never snaps back to the focused column. Focus navigation
 * (scroll_focus_column) slides the view minimally so the target is
 * fully visible; explicit picks (switcher) may center instead. */

static unsigned
tiled_count(wm_t *wm, unsigned idx)
{
    unsigned n = 0;

    for (client_t *c = wm->clients; c; c = c->next)
        if (c->ws == idx && !c->floating && !c->scratchpad &&
            !c->swallowed_by)
            n++;
    return n;
}

/* Index of target among tiled clients, newest-first (list order). */
static int
tiled_index(wm_t *wm, unsigned idx, const client_t *target)
{
    int i = 0;

    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->floating || c->scratchpad ||
            c->swallowed_by)
            continue;
        if (c == target)
            return i;
        i++;
    }
    return -1;
}

static client_t *
tiled_at(wm_t *wm, unsigned idx, int want)
{
    int i = 0;

    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->floating || c->scratchpad ||
            c->swallowed_by)
            continue;
        if (i == want)
            return c;
        i++;
    }
    return NULL;
}

static unsigned
col_pitch(Rect area, unsigned gap, unsigned *col_out)
{
    unsigned aw = area.w > 2 * gap ? area.w - 2 * gap : area.w;

    *col_out = aw / 2;
    return aw + gap;
}

static void
clamp_offset(unsigned n, unsigned pitch, unsigned view_w, unsigned gap,
    workspace_t *ws)
{
    unsigned col = pitch / 2 > gap ? pitch / 2 - gap : pitch / 2;
    unsigned total = n ? (n - 1) * pitch + col : 0;
    unsigned max = total > view_w ? total - view_w : 0;

    if (ws->scroll_off > max)
        ws->scroll_off = max;
}

void
scroll_arrange(wm_t *wm, monitor_t *mon, workspace_t *ws)
{
    unsigned idx = (unsigned)(ws - workspaces);
    Rect area = mon_workarea(mon);
    unsigned gap = cfg.gap;
    int ax = area.x + (int)gap;
    int ay = area.y + (int)gap;
    unsigned aw = area.w > 2 * gap ? area.w - 2 * gap : area.w;
    unsigned ah = area.h > 2 * gap ? area.h - 2 * gap : area.h;
    unsigned col, pitch = col_pitch(area, gap, &col);
    unsigned n = tiled_count(wm, idx);

    if (!n)
        return;
    clamp_offset(n, pitch, aw, gap, ws);

    unsigned i = 0;

    for (client_t *c = wm->clients; c; c = c->next) {
        if (c->ws != idx || c->floating || c->scratchpad ||
            c->swallowed_by)
            continue;
        int x = ax + (int)(i * pitch) - (int)ws->scroll_off;

        apply_geom(wm, c, x, ay, col, ah);
        i++;
    }
}

/* Pan minimally so column `fi` sits fully inside the viewport. */
static void
reveal_column(workspace_t *ws, unsigned pitch, unsigned col,
    unsigned view_w, unsigned fi)
{
    int rel = (int)(fi * pitch) - (int)ws->scroll_off;
    int min_off = (int)(fi * pitch + col) - (int)view_w;

    if (rel < 0)
        ws->scroll_off = fi * pitch;
    else if (min_off > (int)ws->scroll_off)
        ws->scroll_off = (unsigned)min_off;
}

/* niri focus-column-left/right: move focus to the neighbouring tiled
 * client and slide the view so that column is fully visible. */
void
scroll_focus_column(wm_t *wm, int dir)
{
    monitor_t *m = focused_mon(wm);
    workspace_t *ws;
    client_t *cur, *next;
    unsigned gap, col, pitch, view_w;
    int fi;
    Rect area;

    if (!m || !(ws = &workspaces[m->ws_visible]) ||
        austere_layouts[ws->layout_idx].arrange != scroll_arrange)
        return;

    cur = wm->focused && wm->focused->ws == m->ws_visible &&
        !wm->focused->floating ? wm->focused : NULL;
    fi = cur ? tiled_index(wm, m->ws_visible, cur) : -1;
    int n_all = (int)tiled_count(wm, m->ws_visible);
    int want;

    if (fi < 0)
        want = dir > 0 ? 0 : n_all - 1;
    else {
        want = fi + dir;
        if (want < 0 || want >= n_all)
            return;
    }
    next = tiled_at(wm, m->ws_visible, want);

    if (!next)
        return;
    focus(wm, next);

    area = mon_workarea(m);
    gap = cfg.gap;
    pitch = col_pitch(area, gap, &col);
    view_w = area.w > 2 * gap ? area.w - 2 * gap : area.w;
    reveal_column(ws, pitch, col, view_w,
        tiled_index(wm, m->ws_visible, next) < 0
            ? 0
            : (unsigned)tiled_index(wm, m->ws_visible, next));
    clamp_offset(tiled_count(wm, m->ws_visible), pitch, view_w, gap,
        ws);
    arrange(wm);
}

/* Explicit pick (window switcher): center the column in the viewport,
 * clamped to the strip bounds. */
void
scroll_center_client(wm_t *wm, client_t *c)
{
    monitor_t *m;
    workspace_t *ws = &workspaces[c->ws];
    unsigned gap, col, pitch, view_w;
    Rect area;

    if (!c || c->floating ||
        austere_layouts[ws->layout_idx].arrange != scroll_arrange ||
        !(m = ws->mon) || m->ws_visible != c->ws)
        return;

    unsigned idx = (unsigned)(c->ws);

    area = mon_workarea(m);
    gap = cfg.gap;
    pitch = col_pitch(area, gap, &col);
    view_w = area.w > 2 * gap ? area.w - 2 * gap : area.w;

    int fi = tiled_index(wm, idx, c);

    if (fi < 0)
        return;

    int want =
        (int)(fi * pitch + col / 2) - (int)(view_w / 2);

    ws->scroll_off = want > 0 ? (unsigned)want : 0;
    clamp_offset(tiled_count(wm, idx), pitch, view_w, gap, ws);
    arrange(wm);
}

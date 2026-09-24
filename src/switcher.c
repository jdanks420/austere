#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "menu.h"
#include "monitor.h"
#include "settings.h"
#include "layout.h"
#include "switcher.h"
#include "util.h"
#include "workspace.h"

/* §7.2: two switcher modes. MRU quick-cycle walks client MRU order
 * (most-recently-focused first) within the focused monitor's visible
 * workspace; the panel lists every managed client in MRU order and
 * focuses the picked one, switching workspace and monitor as needed. */

void
mru_step(wm_t *wm)
{
    monitor_t *m = focused_mon(wm);

    if (!m || wm->nclients < 2)
        return;
    unsigned vis = m->ws_visible;
    client_t *start = wm->focused;
    client_t *c = start ? start->mru_next : wm->mru;

    for (unsigned k = 0; k < wm->nclients; k++) {
        if (!c)
            c = wm->mru; /* wrap past the tail */
        if (!c)
            break;
        if (c != start && c->ws == vis && !c->scratch_hidden) {
            focus(wm, c);
            return;
        }
        c = c->mru_next;
    }
}

typedef struct {
    xcb_window_t *wins; /* parallel to panel rows */
    char **labels;
    unsigned n;
} switcher_ctx_t;

static switcher_ctx_t sctx;

/* WM_CLASS instance.class read for the label (validated read). */
static char *
win_class(wm_t *wm, xcb_window_t win)
{
    char *cls = wm_class(wm, win);

    return cls ? cls : xstrdup("?");
}

static void
switcher_switch_to(wm_t *wm, const char *row)
{
    if (!row)
        return;
    for (unsigned i = 0; i < sctx.n; i++) {
        if (strcmp(sctx.labels[i], row) != 0)
            continue;
        client_t *c = find_client(wm, sctx.wins[i]);

        if (!c || c->scratch_hidden)
            break;
        if (!ws_shown(c->ws))
            view_ws(wm, c->ws);
        focus(wm, c);
        break;
    }
}

static void
switcher_preview(wm_t *wm, const char *row)
{
    if (!row)
        return;
    monitor_t *m = focused_mon(wm);

    for (unsigned i = 0; i < sctx.n; i++) {
        if (strcmp(sctx.labels[i], row) != 0)
            continue;
        client_t *c = find_client(wm, sctx.wins[i]);

        if (!c || c->scratch_hidden)
            break;
        /* Live preview must not flip the workspace (or monitor) view
         * on every keystroke: jumping to a row on another workspace
         * re-maps and re-tiles the whole screen per press, and cycling
         * back flips again — wall-to-wall damage that reads as flicker
         * under a compositor. Preview only windows already on this
         * monitor's visible workspace; other rows are highlighted and
         * jumped to once, via switcher_enter on Alt release / Return. */
        if (workspaces[c->ws].mon != m || !ws_shown(c->ws))
            break;
        focus(wm, c);
        break;
    }
}

static bool
switcher_enter(wm_t *wm, const char *input, const char *row)
{
    (void)input;
    switcher_switch_to(wm, row);
    return false;
}

static void
switcher_close(wm_t *wm)
{
    (void)wm;
    free(sctx.wins);
    for (unsigned i = 0; i < sctx.n; i++)
        free(sctx.labels[i]);
    free(sctx.labels);
    sctx.wins = NULL;
    sctx.labels = NULL;
    sctx.n = 0;
}

void
switcher_panel_open(wm_t *wm)
{
    monitor_t *m = focused_mon(wm);

    sctx.wins = NULL;
    sctx.labels = NULL;
    sctx.n = 0;
    for (client_t *c = wm->mru; c; c = c->mru_next) {
        if (c->scratch_hidden)
            continue;
        if (cfg.switcher_monitor_scope) {
            monitor_t *home = workspaces[c->ws].mon;

            if (home != m)
                continue;
        }
        xcb_window_t *nw = realloc(sctx.wins,
            (sctx.n + 1) * sizeof(*nw));

        if (!nw)
            break;
        sctx.wins = nw;
        char **nl = realloc(sctx.labels,
            (sctx.n + 1) * sizeof(*nl));

        if (!nl)
            break;
        sctx.labels = nl;
        char *cls = win_class(wm, c->win);
        monitor_t *home = workspaces[c->ws].mon;
        char label[256];

        snprintf(label, sizeof(label), "%.60s - %s - ws %u %s",
            c->name && *c->name ? c->name : "(untitled)",
            cls ? cls : "?", c->ws + 1, home == m ? "*" : "");
        free(cls);
        sctx.labels[sctx.n] = xstrdup(label);
        sctx.wins[sctx.n] = c->win;
        sctx.n++;
    }

    panel_def_t def = {
        .title = "windows",
        .prompt = "",
        .rows = sctx.labels,
        .nrows = sctx.n,
        .filter = true,
        .tab_complete = false,
        .hold_alt = true,
        .init_sel = 1, /* open already pointed at the next MRU window */
        .on_enter = switcher_enter,
        /* live preview: focus+raise the target as the selection moves.
         * With a full-workarea layout (monocle/float) this swaps the
         * whole screen — and re-raises every client — on each keypress,
         * which under a wall-to-wall compositor reads as flicker. Off,
         * the panel just lists (no per-key press swap); the pick still
         * lands via on_enter on Alt release / Return. */
        .on_preview = cfg.switcher_live_preview ? switcher_preview : NULL,
        .on_close = switcher_close,
    };

    panel_open(wm, &def);
}

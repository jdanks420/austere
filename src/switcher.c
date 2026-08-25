#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "menu.h"
#include "monitor.h"
#include "settings.h"
#include "switcher.h"
#include "util.h"
#include "workspace.h"

/* §7.2: two switcher modes. MRU quick-cycle walks the client list
 * (newest-focused first) within the focused monitor's visible
 * workspace; the panel lists every managed client and focuses the
 * picked one, switching workspace and monitor as needed. */

void
mru_step(wm_t *wm)
{
    monitor_t *m = focused_mon(wm);

    if (!m || wm->nclients < 2)
        return;
    unsigned vis = m->ws_visible;
    client_t *start = wm->focused;
    client_t *c = start ? start->next : wm->clients;

    for (;;) {
        if (!c) {
            if (!start)
                break;
            c = wm->clients; /* wrap: list is MRU-ordered */
            if (c == start)
                break;
        }
        if (c == start)
            break;
        if (c->ws == vis && !c->scratch_hidden) {
            focus(wm, c);
            return;
        }
        c = c->next;
    }
}

typedef struct {
    xcb_window_t *wins; /* parallel to panel rows */
    char **labels;
    unsigned n;
} switcher_ctx_t;

static switcher_ctx_t sctx;

/* WM_CLASS instance.class read live for the label (validated read). */
static char *
win_class(wm_t *wm, xcb_window_t win)
{
    size_t len = 0;
    char *raw = (char *)get_property(wm, win, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, &len);

    if (!raw)
        return NULL;
    /* format: instance NUL class NUL */
    char *cls = raw;

    while (*cls && (size_t)(cls - raw) < len)
        cls++;
    if ((size_t)(cls - raw) + 1 < len) {
        cls++;
        char *out = xstrdup(cls);

        free(raw);
        return out;
    }
    free(raw);
    return xstrdup("?");
}

static bool
switcher_enter(wm_t *wm, const char *input, const char *row)
{
    (void)input;
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
    for (client_t *c = wm->clients; c; c = c->next) {
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
        .on_enter = switcher_enter,
        .on_close = switcher_close,
    };

    panel_open(wm, &def);
}

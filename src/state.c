#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "layout.h"
#include "monitor.h"
#include "state.h"
#include "util.h"
#include "workspace.h"

static void
state_path(char *out, unsigned outsz)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");

    if (xdg && *xdg)
        snprintf(out, outsz, "%s/austere/state", xdg);
    else
        snprintf(out, outsz, "/tmp/austere-%u/state",
            (unsigned)getuid());
}

void
state_save(wm_t *wm)
{
    char path[512];

    state_path(path, sizeof(path));
    char *slash = strrchr(path, '/');

    if (slash) {
        *slash = '\0';
        mkdir_p(path);
        *slash = '/';
    }
    FILE *f = fopen(path, "w");

    if (!f)
        return;
    monitor_t *fm = focused_mon(wm);

    fprintf(f, "view %u\n", fm ? fm->ws_visible : 0);
    /* win ws x y w h floating layout_idx split_ratio nmaster */
    for (client_t *c = wm->clients; c; c = c->next)
        fprintf(f, "%u %u %d %d %u %u %d %u %.2f %u\n", (unsigned)c->win,
            c->ws, c->x, c->y, c->w, c->h, c->floating ? 1 : 0,
            workspaces[c->ws].layout_idx, workspaces[c->ws].split_ratio,
            workspaces[c->ws].nmaster);
    fclose(f);
}

void
state_replay(wm_t *wm)
{
    char path[512];

    state_path(path, sizeof(path));
    FILE *f = fopen(path, "r");

    unlink(path); /* consumed either way (§10) */
    if (!f)
        return;

    char line[256];
    unsigned view = 0;

    while (fgets(line, sizeof(line), f)) {
        unsigned win, ws, w, h, layout;
        int x, y, floating;
        double ratio;
        unsigned nmaster;

        if (sscanf(line, "view %u", &view) == 1)
            continue;
        if (sscanf(line, "%u %u %d %d %u %u %d %u %lf %u", &win, &ws,
                &x, &y, &w, &h, &floating, &layout, &ratio,
                &nmaster) != 10)
            continue;
        client_t *c = find_client(wm, (xcb_window_t)win);

        if (!c || ws >= WS_MAX)
            continue;
        if (c->ws != ws)
            send_client_to_ws(wm, c, ws);
        if (floating) {
            c->floating = true;
            c->x = x;
            c->y = y;
            c->w = w;
            c->h = h;
        }
        workspaces[ws].split_ratio = ratio;
        workspaces[ws].nmaster = nmaster;
    }
    fclose(f);
    if (view < WS_MAX && !ws_shown(view))
        view_ws(wm, view);
    arrange(wm);
}

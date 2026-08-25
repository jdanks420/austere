#ifndef AUSTERE_WORKSPACE_H
#define AUSTERE_WORKSPACE_H

#include <stdbool.h>

#include "monitor.h"
#include "wm.h"

#define WS_MAX 9

struct client;
typedef struct client client_t;

typedef struct workspace {
    char *name;          /* conf-provided or decimal fallback */
    monitor_t *mon;      /* output this workspace lives on */
    client_t *sel;       /* last-focused client, NULL if empty */
    unsigned layout_idx; /* active layout for THIS workspace */
    double split_ratio;  /* per-ws layout parameters (SPEC §4.2) */
    unsigned nmaster;
    bool urgent;
} workspace_t;

extern workspace_t workspaces[WS_MAX];

void workspaces_init(wm_t *wm);
void workspaces_shutdown(void);

void view_ws(wm_t *wm, unsigned idx);
void send_focused_to_ws(wm_t *wm, unsigned idx);
void send_client_to_ws(wm_t *wm, struct client *c, unsigned idx);
void ws_migrate_focused_to_next(wm_t *wm);
void client_park(wm_t *wm, struct client *c, bool hide);

#endif

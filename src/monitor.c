#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>

#include "bar.h"
#include "client.h"
#include "monitor.h"
#include "util.h"
#include "workspace.h"

/* Parse "WxH+X+Y" — the AUSTERE_MONS_OVERRIDE / test-poke grammar. */
static int
parse_rect(const char *s, Rect *r)
{
    return sscanf(s, "%ux%u+%d+%d", &r->w, &r->h, &r->x, &r->y) == 4;
}

void
monitors_apply(wm_t *wm, const Rect *rects, unsigned n)
{
    monitor_t *newh = NULL;
    monitor_t **tail = &newh;
    monitor_t *m;

    if (n == 0) {
        Rect fb = { 0, 0, wm->scr->width_in_pixels,
            wm->scr->height_in_pixels };
        rects = &fb;
        n = 1;
    }
    if (n > WS_MAX)
        n = WS_MAX;

    for (unsigned i = 0; i < n; i++) {
        m = xmalloc(sizeof(*m));
        m->geom = rects[i];
        m->ws_visible = 0;
        m->ws_prev = 0;
        m->bar = NULL;
        m->next = NULL;
        *tail = m;
        tail = &m->next;
    }

    if (!wm->mons) {
        /* First enumeration: block-distribute workspaces (i3-style),
         * each monitor showing its lowest-index home. */
        unsigned per = (WS_MAX + n - 1) / n;

        for (unsigned i = 0; i < WS_MAX; i++) {
            unsigned mi = i / per;
            monitor_t *t = newh;

            for (unsigned k = 0; k < mi && t->next; k++)
                t = t->next;
            workspaces[i].mon = t;
        }
        for (m = newh; m; m = m->next) {
            unsigned low = 0;

            while (low < WS_MAX && workspaces[low].mon != m)
                low++;
            if (low == WS_MAX)
                low = 0;
            m->ws_visible = m->ws_prev = low;
        }
        wm->mons = newh;
        bars_sync(wm);
        return;
    }

    /* Carry state across geometries and repoint every workspace at its
     * monitor's replacement. Old objects are all freed below, so no
     * workspace may keep pointing into the previous list. */
    monitor_t *oldh = wm->mons;
    monitor_t *map_old[WS_MAX];
    monitor_t *map_new[WS_MAX];
    int nmap = 0;
    long cx[WS_MAX], cy[WS_MAX];
    int nm = 0;

    for (m = newh; m; m = m->next) {
        cx[nm] = m->geom.x + (long)m->geom.w / 2;
        cy[nm] = m->geom.y + (long)m->geom.h / 2;
        nm++;
    }

    /* Match old↔new by identical geometry, carrying visible/prev. */
    for (monitor_t *o = oldh; o; o = o->next) {
        monitor_t *match = NULL;

        for (m = newh; m; m = m->next) {
            bool taken = false;

            for (int k = 0; k < nmap; k++)
                if (map_new[k] == m)
                    taken = true;
            if (taken)
                continue;
            if (o->geom.x == m->geom.x && o->geom.y == m->geom.y &&
                o->geom.w == m->geom.w && o->geom.h == m->geom.h) {
                match = m;
                break;
            }
        }
        if (!match) {
            /* Orphaned old monitor: nearest new one by center. */
            long best = 0;
            int bi = -1;

            for (int j = 0; j < nm; j++) {
                long dx = cx[j] - (o->geom.x + (long)o->geom.w / 2);
                long dy = cy[j] - (o->geom.y + (long)o->geom.h / 2);
                long d = dx * dx + dy * dy;

                if (bi < 0 || d < best) {
                    best = d;
                    bi = j;
                }
            }
            match = newh;
            for (int k = 0; k < bi && match->next; k++)
                match = match->next;
        } else {
            match->ws_visible = o->ws_visible;
            match->ws_prev = o->ws_prev;
            match->bar = o->bar;
            o->bar = NULL;
        }
        map_old[nmap] = o;
        map_new[nmap] = match;
        nmap++;
    }

    /* Repoint workspaces: surviving geometry keeps its home; the rest
     * re-home to the nearest new monitor (SPEC §5.2 hotplug rule). */
    for (unsigned i = 0; i < WS_MAX; i++) {
        monitor_t *home = workspaces[i].mon;
        monitor_t *t = NULL;

        for (int k = 0; k < nmap; k++)
            if (map_old[k] == home) {
                t = map_new[k];
                break;
            }
        if (!t) {
            long best = 0;
            int bi = -1;

            for (int j = 0; j < nm; j++) {
                long ox = home ? home->geom.x + (long)home->geom.w / 2
                               : 0;
                long oy = home ? home->geom.y + (long)home->geom.h / 2
                               : 0;
                long dx = cx[j] - ox;
                long dy = cy[j] - oy;
                long d = dx * dx + dy * dy;

                if (bi < 0 || d < best) {
                    best = d;
                    bi = j;
                }
            }
            t = newh;
            for (int k = 0; k < bi && t->next; k++)
                t = t->next;
        }
        workspaces[i].mon = t;
    }

    /* The focus cursor must not outlive the list it points into. */
    if (wm->focus_mon) {
        for (int k = 0; k < nmap; k++)
            if (map_old[k] == wm->focus_mon) {
                wm->focus_mon = map_new[k];
                break;
            }
    }

    /* Every monitor needs at least one workspace: steal from the
     * fullest. */
    for (;;) {
        int count[WS_MAX] = { 0 };
        monitor_t *order[WS_MAX];

        int k = 0;

        for (m = newh; m; m = m->next)
            order[k++] = m;
        for (unsigned i = 0; i < WS_MAX; i++)
            for (int j = 0; j < nm; j++)
                if (workspaces[i].mon == order[j])
                    count[j]++;
        int empty = -1, full = -1;

        for (int j = 0; j < nm; j++) {
            if (count[j] == 0)
                empty = j;
            if (count[j] > 1 && (full < 0 || count[j] > count[full]))
                full = j;
        }
        if (empty < 0 || full < 0 || empty == full)
            break;
        for (unsigned i = WS_MAX - 1;; i--) {
            if (workspaces[i].mon == order[full]) {
                workspaces[i].mon = order[empty];
                break;
            }
        }
    }

    /* Repair invariants: visible must reference a ws homed here. */
    for (m = newh; m; m = m->next) {
        unsigned low = 0;

        while (low < WS_MAX && workspaces[low].mon != m)
            low++;
        if (low == WS_MAX)
            continue;
        if (m->ws_visible >= WS_MAX ||
            workspaces[m->ws_visible].mon != m)
            m->ws_visible = low;
        if (m->ws_prev >= WS_MAX ||
            workspaces[m->ws_prev].mon != m ||
            m->ws_prev == m->ws_visible)
            m->ws_prev = m->ws_visible;
    }
    wm->mons = newh;

    while (oldh) {
        m = oldh->next;
        bar_teardown(wm, oldh);
        free(oldh);
        oldh = m;
    }
    bars_sync(wm);
}

monitor_t *
monitors_init(wm_t *wm)
{
    const char *ov = getenv("AUSTERE_MONS_OVERRIDE");
    Rect rects[WS_MAX];
    unsigned n = 0;

    if (ov && *ov) {
        char buf[256];

        snprintf(buf, sizeof(buf), "%s", ov);
        for (char *tok = strtok(buf, ";"); tok && n < WS_MAX;
            tok = strtok(NULL, ";"))
            if (parse_rect(tok, &rects[n]))
                n++;
    } else {
        xcb_randr_get_monitors_cookie_t ck =
            xcb_randr_get_monitors(wm->conn, wm->scr->root, 1);
        xcb_randr_get_monitors_reply_t *r =
            xcb_randr_get_monitors_reply(wm->conn, ck, NULL);

        if (r) {
            xcb_randr_monitor_info_iterator_t it =
                xcb_randr_get_monitors_monitors_iterator(r);

            for (; it.rem && n < WS_MAX;
                xcb_randr_monitor_info_next(&it)) {
                rects[n].x = it.data->x;
                rects[n].y = it.data->y;
                rects[n].w = it.data->width;
                rects[n].h = it.data->height;
                n++;
            }
            free(r);
        }
    }
    monitors_apply(wm, rects, n);
    return wm->mons;
}

void
monitors_refresh(wm_t *wm)
{
    monitors_init(wm);
}

void
monitors_shutdown(wm_t *wm)
{
    monitor_t *m = wm->mons;

    while (m) {
        monitor_t *nxt = m->next;

        free(m);
        m = nxt;
    }
    wm->mons = NULL;
}

monitor_t *
focused_mon(wm_t *wm)
{
    if (wm->focused)
        return workspaces[wm->focused->ws].mon;
    if (wm->focus_mon)
        return wm->focus_mon;
    return wm->mons;
}

bool
ws_shown(unsigned idx)
{
    if (idx >= WS_MAX || !workspaces[idx].mon)
        return false;
    return workspaces[idx].mon->ws_visible == idx;
}

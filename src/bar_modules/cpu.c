#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../bar.h"
#include "../barmod.h"
#include "../util.h"

/* /proc/stat delta between two reads (§6.2): "cpu {pct}%". The
 * 1 s poll tick wakes one read per second; the first frame shows 0%
 * because there is no earlier snapshot. */

typedef struct {
    unsigned long long prev_total, prev_idle;
    time_t last;
    bool have;
    char text[16];
} cpu_state_t;

static void
cpu_read(cpu_state_t *st)
{
    unsigned long long v[8] = { 0 };

    st->text[0] = '\0';
    FILE *f = fopen("/proc/stat", "r");

    if (!f)
        return;
    char line[256];

    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }
    fclose(f);
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);

    if (n < 4)
        return;
    unsigned long long idle = v[3] + (n > 4 ? v[4] : 0);
    unsigned long long total = 0;
    unsigned pct = 0;

    for (int i = 0; i < n; i++)
        total += v[i];
    if (st->have && total > st->prev_total) {
        unsigned long long dtot = total - st->prev_total;
        unsigned long long didle = idle - st->prev_idle;

        if (dtot && didle < dtot)
            pct = (unsigned)((dtot - didle) * 100 / dtot);
    }
    st->prev_total = total;
    st->prev_idle = idle;
    st->have = true;
    snprintf(st->text, sizeof(st->text), "cpu %u%%", pct);
}

static unsigned
cpumod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    cpu_state_t *st = m->data;
    time_t now = time(NULL);

    if (now != (st ? st->last : 0)) {
        if (!st) {
            st = xmalloc(sizeof(*st));
            memset(st, 0, sizeof(*st));
            m->data = st;
        }
        st->last = now;
        cpu_read(st);
    }
    return textmod_render(wm, mon, m, x, draw, st->text,
        strlen(st->text));
}

const mod_reg_t cpumod = { "cpu", cpumod_render, mod_noop_click };
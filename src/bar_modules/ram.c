#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../bar.h"
#include "../barmod.h"

/* /proc/meminfo (§6.2): "ram {pct}%" of MemTotal consumed, via
 * MemAvailable when the kernel exposes it. Lazy refresh every second. */

#define RAM_INTERVAL 1

static void
ram_read(char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *f = fopen("/proc/meminfo", "r");

    if (!f)
        return;
    char line[128];
    unsigned long long total = 0, avail = 0;

    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;

        if (sscanf(line, "MemTotal: %llu kB", &v) == 1)
            total = v;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1)
            avail = v;
        if (total && avail)
            break;
    }
    fclose(f);
    if (!total)
        return;
    unsigned used = avail <= total
        ? (unsigned)((total - avail) * 100 / total)
        : 100;

    snprintf(out, outsz, "ram %u%%", used);
}

static unsigned
rammod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    static time_t last;
    static char text[16];

    if (time(NULL) - last >= RAM_INTERVAL) {
        last = time(NULL);
        ram_read(text, sizeof(text));
    }
    return textmod_render(wm, mon, m, x, draw, text, strlen(text));
}

const mod_reg_t rammod = { "ram", rammod_render, mod_noop_click };
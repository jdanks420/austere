#define _POSIX_C_SOURCE 200809L

#include <time.h>

#include "../bar.h"
#include "../settings.h"
#include "../barmod.h"
#include "../draw.h"

/* strftime clock (§6.2); the minute tick arrives via the poll timeout,
 * never a timer of our own. */

static unsigned
clockmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    char buf[32];
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    size_t len = strftime(buf, sizeof(buf),
        cfg.time_format ? cfg.time_format : "%a %d %H:%M", &tm);

    return textmod_render(wm, mon, m, x, draw, buf, len);
}

const mod_reg_t clockmod = { "clock", clockmod_render, mod_noop_click };

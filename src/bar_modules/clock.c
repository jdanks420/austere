#define _POSIX_C_SOURCE 200809L

#include <time.h>

#include "../bar.h"
#include "../settings.h"
#include "../barmod.h"
#include "../draw.h"

/* strftime clock (§6.2); the minute tick arrives via the poll timeout,
 * never a timer of our own. */

unsigned
clockmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    font_t *f = bar_font(wm, m);
    char buf[32];
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    size_t len = strftime(buf, sizeof(buf),
        cfg.time_format ? cfg.time_format : "%a %d %H:%M", &tm);
    unsigned w = draw_text_w(wm, f, buf, (unsigned)len) + 2 * BAR_PAD;

    if (!draw)
        return w;
    draw_text(wm, &mon->bar->draw, f, x + BAR_PAD,
        bar_baseline(wm, mon, f), buf, (unsigned)len,
        bar_color(m, BAR_FG), BAR_BG);
    return w;
}

void
clockmod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x,
    int px, unsigned btn)
{
    (void)wm;
    (void)mon;
    (void)m;
    (void)mod_x;
    (void)px;
    (void)btn;
}

const mod_reg_t clockmod = { "clock", clockmod_render,
    clockmod_click };

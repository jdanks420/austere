#include <string.h>

#include "../bar.h"
#include "../barmod.h"
#include "../client.h"
#include "../draw.h"
#include "../settings.h"

/* Focused client's title, ellipsized when oversized (§6.2).
 * Occupies no reserved width — it floats after the left group and is
 * clipped to a quarter of the bar. Clicks ignored. */

static unsigned
titlemod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    font_t *f = bar_font(wm, m);
    client_t *c = wm->focused;
    const char *name;

    if (!c || !ws_shown(c->ws) || !c->name || !*c->name)
        return 0;
    name = c->name;
    size_t len = strlen(name);
    size_t full = len;
    unsigned avail = mon->geom.w / 4;

    while (len &&
        draw_text_w(wm, f, name, (unsigned)len) + 2 * BAR_PAD > avail)
        len--;
    if (!len)
        return 0;
    unsigned w = draw_text_w(wm, f, name, (unsigned)len) +
        (len < full ? draw_text_w(wm, f, "...", 3) : 0) + 2 * BAR_PAD;

    if (draw && x >= 0) {
        draw_text(wm, &mon->bar->draw, f, x + BAR_PAD,
            bar_baseline(wm, mon, f), name, (unsigned)len,
            bar_color(m, cfg.bar_fg), cfg.bar_bg);
        if (len < full)
            draw_text(wm, &mon->bar->draw, bar_font(wm, NULL),
                x + (int)BAR_PAD + (int)draw_text_w(wm, f, name,
                    (unsigned)len),
                bar_baseline(wm, mon, bar_font(wm, NULL)), "...", 3,
                BAR_DIM, cfg.bar_bg);
    }
    return w;
}

static void
titlemod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x, int px,
    unsigned btn)
{
    (void)wm;
    (void)mon;
    (void)m;
    (void)mod_x;
    (void)px;
    (void)btn;
}

const mod_reg_t titlemod = { "title", titlemod_render,
    titlemod_click };

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../bar.h"
#include "../barmod.h"
#include "../draw.h"

/* /sys/class/power_supply scan (§6.2): "{state} {cap}%". Lazy refresh
 * every 30 s; renders empty on machines without a battery. */

#define BAT_INTERVAL 30

static void
bat_read(char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *f = fopen("/sys/class/power_supply/BAT0/uevent", "r");

    if (!f)
        return;
    char line[128];
    char cap[8] = "";
    char state[16] = "";

    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "POWER_SUPPLY_STATUS=%15s", state);
        sscanf(line, "POWER_SUPPLY_CAPACITY=%7s", cap);
    }
    fclose(f);
    if (!cap[0])
        return;
    snprintf(out, outsz, "%s %s%%", state[0] ? state : "Bat", cap);
}

unsigned
batmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    static time_t last;
    static char text[32];
    font_t *f = bar_font(wm, m);

    if (time(NULL) - last >= BAT_INTERVAL) {
        last = time(NULL);
        bat_read(text, sizeof(text));
    }
    size_t len = strlen(text);

    if (!len || !draw)
        return len ? draw_text_w(wm, f, text, (unsigned)len) +
                           2 * BAR_PAD
                   : 0;
    unsigned w = draw_text_w(wm, f, text, (unsigned)len) + 2 * BAR_PAD;

    draw_text(wm, &mon->bar->draw, f, x + BAR_PAD,
        bar_baseline(wm, mon, f), text, (unsigned)len,
        bar_color(m, BAR_FG), BAR_BG);
    return w;
}

void
batmod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x, int px,
    unsigned btn)
{
    (void)wm;
    (void)mon;
    (void)m;
    (void)mod_x;
    (void)px;
    (void)btn;
}

const mod_reg_t batmod = { "battery", batmod_render,
    batmod_click };

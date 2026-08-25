#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../bar.h"
#include "../barmod.h"
#include "../draw.h"

/* Shells out to pactl, falling back to amixer (§6.2) — deliberately
 * impure: no uniform mixer ABI exists without libasound, which we
 * refuse to link. Lazy refresh every 5 s. */

#define VOL_INTERVAL 5

/* First "N%" token anywhere in the stream covers both tools' output. */
static bool
first_pct(const char *s, char *out, size_t outsz)
{
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            unsigned i = 0;

            while (s[i] >= '0' && s[i] <= '9' && i < 4)
                i++;
            if (s[i] == '%') {
                char tmp[8];

                memcpy(tmp, s, i);
                tmp[i++] = '%';
                tmp[i] = '\0';
                snprintf(out, outsz, "%s", tmp);
                return true;
            }
        }
        s++;
    }
    return false;
}

static void
vol_read(char *out, size_t outsz)
{
    char line[512];

    out[0] = '\0';
    FILE *f = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null",
        "r");

    if (f) {
        if (fgets(line, sizeof(line), f))
            first_pct(line, out, outsz);
        pclose(f);
    }
    if (!out[0]) {
        f = popen("amixer get Master 2>/dev/null", "r");
        if (f) {
            while (fgets(line, sizeof(line), f))
                if (first_pct(line, out, outsz))
                    break;
            pclose(f);
        }
    }
}

unsigned
volmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    static time_t last;
    static char text[16];
    font_t *f = bar_font(wm, m);

    if (time(NULL) - last >= VOL_INTERVAL) {
        last = time(NULL);
        vol_read(text, sizeof(text));
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
volmod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x, int px,
    unsigned btn)
{
    (void)wm;
    (void)mon;
    (void)m;
    (void)mod_x;
    (void)px;
    (void)btn;
}

const mod_reg_t volmod = { "volume", volmod_render,
    volmod_click };

#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include "../bar.h"
#include "../barmod.h"
#include "../draw.h"
#include "../settings.h"
#include <stdio.h>

#include "../workspace.h"

/* Click-to-view pager: the focused workspace accented, other monitors'
 * visibles dimmed, urgent workspaces tinted (§6.2). */

static void
label_for(unsigned i, char *buf)
{
    const char *name = workspaces[i].name;

    if (name && *name) {
        snprintf(buf, 16, "%s", name);
        return;
    }
    buf[0] = (char)('1' + i);
    buf[1] = '\0';
}

/* Shared layout walk so click hit-testing always matches the frame.
 * With draw=false only the total width is computed; rel_x >= 0 then
 * picks a workspace index into *hit. */
static unsigned
walk(wm_t *wm, monitor_t *mon, int x, bool draw, int rel_x, int *hit,
    font_t *f, uint32_t base_col)
{
    monitor_t *fm = focused_mon(wm);
    unsigned pad = BAR_PAD;
    int cx = 0;

    if (hit)
        *hit = -1;
    for (unsigned i = 0; i < WS_MAX; i++) {
        char buf[16];

        label_for(i, buf);
        size_t len = strlen(buf);
        unsigned w = draw_text_w(wm, f, buf, (unsigned)len) + 2 * pad;

        if (draw) {
            uint32_t col = base_col;

            if (ws_shown(i))
                col = fm && fm->ws_visible == i ? cfg.focus_color : BAR_DIM;
            if (workspaces[i].urgent)
                col = cfg.urgent_color;
            draw_text(wm, &mon->bar->draw, f, x + cx + (int)pad,
                bar_baseline(wm, mon, f), buf, (unsigned)len, col, cfg.bar_bg);
        } else if (rel_x >= cx && rel_x < (int)(cx + w)) {
            *hit = (int)i;
        }
        cx += (int)w;
    }
    return (unsigned)cx;
}

static unsigned
wsmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    font_t *f = bar_font(wm, m);

    return walk(wm, mon, x, draw, -1, NULL, f,
        bar_color(m, cfg.bar_fg));
}

static void
wsmod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x, int px,
    unsigned btn)
{
    int hit;

    (void)mon;
    (void)m;
    (void)mod_x;
    /* px is the pointer position relative to the module start, so the
     * walk's cx offsets line up with rel_x */
    if (btn != XCB_BUTTON_INDEX_1)
        return;
    walk(wm, mon, 0, false, px, &hit, bar_font(wm, m), 0);
    if (hit >= 0)
        view_ws(wm, (unsigned)hit);
}

const mod_reg_t wsmod = { "workspaces", wsmod_render,
    wsmod_click };

#include <string.h>

#include "../bar.h"
#include "../barmod.h"
#include "../draw.h"
#include "../layout.h"
#include "../workspace.h"

/* Current layout glyph (SPEC §4.4); click cycles layouts. */

unsigned
layoutmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw)
{
    font_t *f = bar_font(wm, m);
    workspace_t *ws = &workspaces[mon->ws_visible];
    const char *sym = austere_layouts[ws->layout_idx].symbol;
    size_t len = strlen(sym);
    unsigned w = draw_text_w(wm, f, sym, (unsigned)len) + 2 * BAR_PAD;

    if (!draw)
        return w;
    draw_text(wm, &mon->bar->draw, f, x + BAR_PAD,
        bar_baseline(wm, mon, f), sym, (unsigned)len,
        bar_color(m, BAR_FG), BAR_BG);
    return w;
}

void
layoutmod_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x,
    int px, unsigned btn)
{
    (void)mon;
    (void)m;
    (void)mod_x;
    (void)px;
    if (btn == XCB_BUTTON_INDEX_1)
        cycle_layout(wm);
}

const mod_reg_t layoutmod = { "layout", layoutmod_render,
    layoutmod_click };

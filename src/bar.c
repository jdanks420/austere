#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atoms.h"
#include "bar.h"
#include "barmod.h"
#include "settings.h"
#include "client.h"
#include "draw.h"
#include "ewmh.h"
#include "menu.h"
#include "module.h"
#include "popup.h"
#include "util.h"
#include "workspace.h"

#define BAR_HEIGHT_PAD 2

/* Hardcoded until M7 wires [bar] left/right ordering. */
static const char *DEFAULT_LEFT[] = { "workspaces", "layout", "title",
    NULL };
static const char *DEFAULT_RIGHT[] = { "clock", "battery", "volume",
    NULL };

font_t *
bar_font(wm_t *wm, module_t *m)
{
    if (m && m->font)
        return m->font;
    return draw_ui_font(wm);
}

uint32_t
bar_color(module_t *m, uint32_t fallback)
{
    return m && m->color ? m->color : fallback;
}

int
bar_baseline(wm_t *wm, monitor_t *mon, font_t *f)
{
    (void)wm;
    return (int)((mon->bar->height - font_height(f)) / 2) +
        (int)font_ascent(f);
}

static void
mod_clear(module_t *m)
{
    memset(m, 0, sizeof(*m));
    m->fd = -1;
}

static bool
mod_init_builtin(module_t *m, const char *type)
{
    const mod_reg_t *r = mod_lookup(type);

    if (!r) {
        fprintf(stderr, "austere: unknown bar module '%s'\n", type);
        return false;
    }
    mod_clear(m);
    snprintf(m->name, sizeof(m->name), "%s", type);
    m->type = r->type;
    return true;
}

/* AUSTERE_BAR_SCRIPTS="name:/path/x.sh;other:y.sh": dev seam so script
 * modules are exercisable before M7 delivers [bar.module.*] conf
 * sections. Instances attach to the head monitor's right group. */
static void
bar_scripts_from_env(wm_t *wm)
{
    const char *spec = getenv("AUSTERE_BAR_SCRIPTS");
    char buf[512];

    if (!spec || !*spec || !wm->mons)
        return;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        char *colon = strchr(tok, ':');

        if (!colon || !*colon || colon == tok)
            continue;
        *colon = '\0';
        module_t *m =
            &wm->mons->bar->mods[wm->mons->bar->nmods];

        if (wm->mons->bar->nmods >= BAR_MAX_MODULES)
            return;
        mod_clear(m);
        snprintf(m->name, sizeof(m->name), "%s", tok);
        m->exec = xstrdup(colon + 1);
        wm->mons->bar->nmods++;
    }
}

static void
bar_attach(wm_t *wm, monitor_t *mo)
{
    (void)wm;
    bar_t *b = xmalloc(sizeof(*b));

    memset(b, 0, sizeof(*b));
    b->win = XCB_NONE;
    for (unsigned i = 0; i < BAR_MAX_MODULES; i++)
        b->mods[i].fd = -1;
    unsigned idx = 0;

    while (DEFAULT_LEFT[idx] && idx < BAR_MAX_MODULES &&
        mod_init_builtin(&b->mods[idx], DEFAULT_LEFT[idx]))
        idx++;
    b->nleft = idx;
    b->nmods = idx;
    for (unsigned s = 0;
        DEFAULT_RIGHT[s] && b->nmods < BAR_MAX_MODULES; s++)
        if (mod_init_builtin(&b->mods[b->nmods], DEFAULT_RIGHT[s]))
            b->nmods++;
    mo->bar = b;
}

/* bars_sync (via monitors_apply) has already attached every monitor's
 * bar by the time we run; bar_init only layers on script instances. */
void
bar_init(wm_t *wm)
{
    bar_scripts_from_env(wm);

    for (monitor_t *mo = wm->mons; mo; mo = mo->next)
        for (unsigned i = 0; i < mo->bar->nmods; i++)
            if (mo->bar->mods[i].exec)
                module_spawn_script(wm, &mo->bar->mods[i]);
}

Rect
mon_workarea(const monitor_t *m)
{
    Rect r = m->geom;

    if (m->bar && m->bar->mapped && r.h > m->bar->height) {
        if (cfg.bar_bottom)
            r.h -= m->bar->height;
        else {
            r.y += (int)m->bar->height;
            r.h -= m->bar->height;
        }
    }
    return r;
}

static void
bar_create_window(wm_t *wm, monitor_t *mon)
{
    bar_t *b = mon->bar;
    font_t *f = draw_ui_font(wm);

    b->height = font_height(f) + 2 * BAR_HEIGHT_PAD;
    int by = cfg.bar_bottom
        ? (int)(mon->geom.y + mon->geom.h - b->height)
        : mon->geom.y;
    b->win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, b->win,
        wm->scr->root, (int16_t)mon->geom.x, (int16_t)by,
        (uint16_t)mon->geom.w, (uint16_t)b->height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
            XCB_CW_EVENT_MASK,
        (uint32_t[]){ BAR_BG, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS });
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, b->win,
        wm->atoms->net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
        (const uint32_t[]){ wm->atoms->net_wm_window_type_dock });
    draw_setup(wm, &b->draw, b->win);
    xcb_map_window(wm->conn, b->win);
    b->mapped = true;
}

/* Topology changed: create bars for new monitors, resize/move existing,
 * repaint. Monitors_apply carries surviving bar pointers across. */
void
bars_sync(wm_t *wm)
{
    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar)
            bar_attach(wm, m);
        bar_t *b = m->bar;
        b->height = font_height(draw_ui_font(wm)) +
            2 * BAR_HEIGHT_PAD;
        int by = cfg.bar_bottom
            ? (int)(m->geom.y + m->geom.h - b->height)
            : m->geom.y;
        uint32_t vals[] = { (uint32_t)m->geom.x, (uint32_t)by,
            (uint32_t)m->geom.w, b->height };

        if (b->win == XCB_NONE)
            bar_create_window(wm, m);
        else
            xcb_configure_window(wm->conn, b->win,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH |
                    XCB_CONFIG_WINDOW_HEIGHT,
                vals);
        b->mapped = true;
    }
    bar_render_all(wm);
    ewmh_update_workarea(wm);
}

/* Left group then separator-daisy-chained right group. Script modules
 * render their last frame; dead ones keep it frozen (§6.3). */
void
bar_render(wm_t *wm, monitor_t *mon)
{
    bar_t *b = mon->bar;

    if (!b || b->win == XCB_NONE)
        return;
    draw_rect(wm, &b->draw, 0, 0, mon->geom.w, b->height, BAR_BG);

    int x = BAR_PAD;
    bool first = true;

    for (unsigned i = 0; i < b->nleft; i++) {
        module_t *m = &b->mods[i];
        const mod_reg_t *reg = mod_lookup(m->type);

        if (!reg || !reg->render)
            continue;
        unsigned w = reg->render(wm, mon, m, x, true);

        if (w) {
            if (!first)
                x += BAR_PAD;
            x += (int)w + BAR_PAD;
            first = false;
        }
    }

    /* Right group: measure everything, then place from the edge. */
    unsigned widths[BAR_MAX_MODULES] = { 0 };
    unsigned total = 0;

    for (unsigned i = b->nleft; i < b->nmods; i++) {
        module_t *m = &b->mods[i];
        const mod_reg_t *reg = mod_lookup(m->type);
        unsigned w = 0;

        if (m->exec) {
            if (m->flen)
                w = draw_text_w(wm, bar_font(wm, m), m->frame,
                    (unsigned)m->flen) + 2 * BAR_PAD;
        } else if (reg && reg->render) {
            w = reg->render(wm, mon, m, -10000, false);
        }
        widths[i] = w;
        total += w ? w + BAR_PAD : 0;
    }

    font_t *df = draw_ui_font(wm);
    unsigned sepw = draw_text_w(wm, df, "|", 1);
    int rx = (int)(mon->geom.w - total - BAR_PAD);

    /* positions first, then separators centered in each gap */
    int xs[BAR_MAX_MODULES] = { 0 };
    unsigned prev_end = 0;
    bool have_prev = false;

    for (unsigned i = b->nleft; i < b->nmods; i++) {
        if (!widths[i])
            continue;
        xs[i] = rx;
        if (have_prev) {
            int gap_l = (int)prev_end + BAR_PAD;
            int gap_r = rx + BAR_PAD;
            int sx = (gap_l + gap_r) / 2 - (int)sepw / 2;

            draw_text(wm, &b->draw, df, sx, bar_baseline(wm, mon, df),
                "|", 1, BAR_DIM, BAR_BG);
        }
        prev_end = rx + (int)widths[i] - BAR_PAD;
        have_prev = true;
        rx += (int)(widths[i] + BAR_PAD);
    }

    for (unsigned i = b->nleft; i < b->nmods; i++) {
        module_t *m = &b->mods[i];
        const mod_reg_t *reg = mod_lookup(m->type);

        if (!widths[i])
            continue;
        if (m->exec) {
            draw_text(wm, &b->draw, bar_font(wm, m),
                xs[i] + (int)BAR_PAD,
                bar_baseline(wm, mon, bar_font(wm, m)), m->frame,
                (unsigned)m->flen, bar_color(m, BAR_FG), BAR_BG);
        } else if (reg && reg->render) {
            reg->render(wm, mon, m, xs[i], true);
        }
    }
}

void
bar_render_all(wm_t *wm)
{
    for (monitor_t *m = wm->mons; m; m = m->next)
        bar_render(wm, m);
}

/* Milliseconds until the clock needs a refresh; -1 when no clock is
 * configured. Called from the poll loop as the timeout. */
int
bar_timeout_ms(wm_t *wm)
{
    time_t now = time(NULL);

    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar)
            continue;
        for (unsigned i = 0; i < m->bar->nmods; i++)
            if (m->bar->mods[i].type &&
                !strcmp(m->bar->mods[i].type, "clock"))
                return (int)((60 - now % 60) * 1000) + 250;
    }
    return -1;
}

bool
bar_button(wm_t *wm, xcb_window_t win, int px, unsigned btn)
{
    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar || m->bar->win != win)
            continue;
        /* hit-testing walks the left group only: right-group modules
         * are placed from the right edge and none take clicks */
        int x = BAR_PAD;

        for (unsigned i = 0; i < m->bar->nleft; i++) {
            module_t *mod = &m->bar->mods[i];
            const mod_reg_t *reg = mod_lookup(mod->type);
            unsigned w = 0;

            if (mod->exec) {
                if (mod->flen)
                    w = draw_text_w(wm, bar_font(wm, mod), mod->frame,
                        (unsigned)mod->flen) + 2 * BAR_PAD;
            } else if (reg && reg->render) {
                w = reg->render(wm, m, mod, -10000, false);
            }
            if (!w)
                continue;
            if (px >= x && px < x + (int)(w + BAR_PAD)) {
                if (btn == XCB_BUTTON_INDEX_3)
                    menu_open(wm); /* §6.4: right-click = settings */
                else if (reg->click)
                    reg->click(wm, m, mod, x, px, btn);
                return true;
            }
            x += (int)(w + BAR_PAD);
        }
        return true;
    }
    return false;
}

void
bar_pump_fd(wm_t *wm, int fd)
{
    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar)
            continue;
        for (unsigned i = 0; i < m->bar->nmods; i++)
            if (m->bar->mods[i].fd == fd) {
                module_pump(wm, &m->bar->mods[i]);
                return;
            }
    }
}

void
bar_collect_fds(wm_t *wm, struct pollfd *fds, unsigned *n, unsigned max)
{
    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar)
            continue;
        for (unsigned i = 0; i < m->bar->nmods; i++) {
            module_t *mod = &m->bar->mods[i];

            if (*n < max && mod->fd >= 0 && !mod->dead) {
                fds[*n].fd = mod->fd;
                fds[*n].events = POLLIN;
                (*n)++;
            }
        }
    }
}

/* Destroy a monitor's bar entirely (monitor leaving the topology). */
void
bar_teardown(wm_t *wm, monitor_t *m)
{
    if (!m->bar)
        return;
    for (unsigned i = 0; i < m->bar->nmods; i++)
        module_kill(wm, &m->bar->mods[i]);
    if (m->bar->win != XCB_NONE)
        xcb_destroy_window(wm->conn, m->bar->win);
    free(m->bar);
    m->bar = NULL;
}

void
bars_shutdown(wm_t *wm)
{
    for (monitor_t *m = wm->mons; m; m = m->next)
        bar_teardown(wm, m);
}

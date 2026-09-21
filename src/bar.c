#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atoms.h"
#include "apps.h"
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

#include "logo.h"

#define BAR_HEIGHT_PAD 2
#define LOGO_PAD 4

/* Default placement; [bar] modules_left/center/right override each
 * group individually (SPEC §6.4). */
static const char *DEFAULT_LEFT[] = { "workspaces", "layout", NULL };
static const char *DEFAULT_CENTER[] = { "title", NULL };
static const char *DEFAULT_RIGHT[] = { "cpu", "ram", "battery", "volume",
    "clock", NULL };

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

/* Shared render tail for read-a-string-then-draw modules: measures or
 * draws a single text frame and returns its padded width. */
unsigned
textmod_render(wm_t *wm, monitor_t *mon, module_t *m, int x, bool draw,
    const char *text, size_t len)
{
    font_t *f = bar_font(wm, m);

    if (!len || !draw)
        return len ? draw_text_w(wm, f, text, (unsigned)len) +
                            2 * BAR_PAD
                   : 0;
    unsigned w = draw_text_w(wm, f, text, (unsigned)len) + 2 * BAR_PAD;

    draw_text(wm, &mon->bar->draw, f, x + BAR_PAD,
        bar_baseline(wm, mon, f), text, (unsigned)len,
        bar_color(m, cfg.bar_fg), cfg.bar_bg);
    return w;
}

void
mod_noop_click(wm_t *wm, monitor_t *mon, module_t *m, int mod_x, int px,
    unsigned btn)
{
    (void)wm;
    (void)mon;
    (void)m;
    (void)mod_x;
    (void)px;
    (void)btn;
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

/* AUSTERE_BAR_SCRIPTS="name:/path/x.sh;other:y.sh": attach script
 * modules on top of the configured bar so they can be exercised before
 * a `[bar.module.<name>]` conf section (if any) is defined. Instances
 * attach to the head monitor's right group. */
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

/* Append one placement group's modules; a NULL/empty settings list
 * falls back to the DEFAULT_* table. Returns the requested count
 * (before BAR_MAX_MODULES truncation). */
static unsigned
bar_fill_group(bar_t *b, char **want, unsigned nwant,
    const char *const fallback[])
{
    const char *const *list = fallback;
    unsigned n;

    if (want && nwant) {
        list = (const char *const *)want;
        n = nwant;
    } else {
        for (n = 0; fallback[n]; n++)
            ;
    }
    for (unsigned i = 0; i < n && b->nmods < BAR_MAX_MODULES; i++)
        if (mod_init_builtin(&b->mods[b->nmods], list[i]))
            b->nmods++;
    return n;
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
    bar_fill_group(b, cfg.bar_left, cfg.nbar_left, DEFAULT_LEFT);
    b->nleft = b->nmods;
    bar_fill_group(b, cfg.bar_center, cfg.nbar_center, DEFAULT_CENTER);
    b->ncenter = b->nmods - b->nleft;
    bar_fill_group(b, cfg.bar_right, cfg.nbar_right, DEFAULT_RIGHT);
    mo->bar = b;
}

/* True when the bar's builtin sequence no longer matches what the
 * current settings demand (extra trailing script modules are slack). */
static bool
bar_wants_rebuild(bar_t *b)
{
    const char *expect[BAR_MAX_MODULES];
    unsigned n = 0;

    for (unsigned g = 0; g < 3; g++) {
        char **want = g == 0 ? cfg.bar_left
                       : g == 1 ? cfg.bar_center
                                : cfg.bar_right;
        unsigned nwant = g == 0 ? cfg.nbar_left
                         : g == 1 ? cfg.nbar_center
                                  : cfg.nbar_right;
        const char *const *fb = g == 0 ? DEFAULT_LEFT
                                : g == 1 ? DEFAULT_CENTER
                                         : DEFAULT_RIGHT;

        if (want && nwant)
            for (unsigned i = 0; i < nwant && n < BAR_MAX_MODULES; i++)
                expect[n++] = want[i];
        else
            for (unsigned i = 0; fb[i] && n < BAR_MAX_MODULES; i++)
                expect[n++] = fb[i];
    }
    if (n > b->nmods)
        return true;
    for (unsigned i = 0; i < n; i++) {
        module_t *m = &b->mods[i];

        if (m->exec || !m->type || strcmp(m->type, expect[i]))
            return true;
    }
    return false;
}

/* Re-attach the env script modules to the head monitor's bar. Used at
 * boot (bar_init) and again when a placement-triggered rebuild drops
 * them (§6.4 live-apply). */
static void
bar_seed_scripts(wm_t *wm)
{
    if (!wm->mons || !wm->mons->bar)
        return;
    bar_scripts_from_env(wm);
    for (unsigned i = 0; i < wm->mons->bar->nmods; i++)
        if (wm->mons->bar->mods[i].exec)
            module_spawn_script(wm, &wm->mons->bar->mods[i]);
}

/* bars_sync (via monitors_apply) has already attached every monitor's
 * bar by the time we run; bar_init only layers on script instances. */
void
bar_init(wm_t *wm)
{
    bar_seed_scripts(wm);
}

Rect
mon_workarea(const monitor_t *m)
{
    Rect r = m->geom;

    if (m->bar && m->bar->mapped && r.h > m->bar->height) {
        if (cfg.bar_bottom)
            r.h -= (int)(m->bar->height + cfg.bar_gap);
        else {
            r.y += (int)(m->bar->height + cfg.bar_gap);
            r.h -= (int)(m->bar->height + cfg.bar_gap);
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
        ? (int)(mon->geom.y + mon->geom.h - b->height - cfg.bar_gap)
        : (int)(mon->geom.y + cfg.bar_gap);
    b->win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, b->win,
        wm->scr->root, (int16_t)(mon->geom.x + cfg.bar_gap), (int16_t)by,
        (uint16_t)(mon->geom.w - 2 * cfg.bar_gap), (uint16_t)b->height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
            XCB_CW_EVENT_MASK,
        (uint32_t[]){ cfg.bar_bg, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS });
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, b->win,
        wm->atoms->net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
        (const uint32_t[]){ wm->atoms->net_wm_window_type_dock });
    draw_setup(wm, &b->draw, b->win);
    /* mapped later by bars_sync, after the first render: compositors
     * snapshot at MapNotify and would freeze a blank frame */
}

/* Topology changed: create bars for new monitors, resize/move existing,
 * repaint. Monitors_apply carries surviving bar pointers across. */
/* Topology changed: create bars for new monitors, resize/move existing,
 * repaint. Monitors_apply carries surviving bar pointers across. A
 * deliberate module-placement change rebuilds the affected bars on the
 * spot. */
void
bars_sync(wm_t *wm)
{
    bool head_rebuilt = false;

    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (m->bar && bar_wants_rebuild(m->bar)) {
            if (m == wm->mons)
                head_rebuilt = true;
            bar_teardown(wm, m);
            m->bar = NULL;
        }
        if (!m->bar)
            bar_attach(wm, m);
        bar_t *b = m->bar;
        b->height = font_height(draw_ui_font(wm)) +
            2 * BAR_HEIGHT_PAD;
        int by = cfg.bar_bottom
            ? (int)(m->geom.y + m->geom.h - b->height - cfg.bar_gap)
            : (int)(m->geom.y + cfg.bar_gap);
        uint32_t vals[] = { (uint32_t)(m->geom.x + cfg.bar_gap), (uint32_t)by,
            (uint32_t)(m->geom.w - 2 * cfg.bar_gap), b->height };

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
    if (head_rebuilt)
        bar_seed_scripts(wm);
    /* paint first, then raise the curtain (see bar_create_window) */
    bar_render_all(wm);
    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (m->bar && !m->bar->on_screen) {
            xcb_map_window(wm->conn, m->bar->win);
            m->bar->on_screen = true;
        }
        if (m->bar)
            m->bar->mapped = true;
    }
    ewmh_update_workarea(wm);
}

/* Logo at the far left: alpha-blend the white or black variant onto
 * the live bar background depending on which contrasts harder. Painted
 * as filled 1x1 cells with the working GC path rather than put_image:
 * the thin strip must survive byte-order and depth roulette on 24-bit
 * servers, and cells use the same primitive the text modules render
 * with. */
static void
bar_draw_logo(wm_t *wm, monitor_t *mon)
{
    unsigned bgv = cfg.bar_bg;
    unsigned br = (bgv >> 16) & 0xff, bg = (bgv >> 8) & 0xff,
        bb = bgv & 0xff;
    const uint8_t *px =
        (br + bg + bb) < 384 ? logo_white : logo_black;
    uint32_t out[LOGO_W * LOGO_H];
    int oy = ((int)mon->bar->height - (int)LOGO_H) / 2;

    if (oy < 0)
        oy = 0;
    for (unsigned i = 0; i < LOGO_W * LOGO_H; i++) {
        unsigned a = px[i * 4 + 3];
        unsigned r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];

        out[i] = ((r * a + br * (255 - a) + 127) / 255 << 16) |
            ((g * a + bg * (255 - a) + 127) / 255 << 8) |
            (b * a + bb * (255 - a) + 127) / 255;
    }
    draw_put_image24(wm, mon->bar->win, mon->bar->draw.gc,
        (uint8_t)wm->scr->root_depth, (int16_t)LOGO_PAD, (int16_t)oy,
        LOGO_W, LOGO_H, out);
}

/* Left, then a measured-and-centered group, then the separator-
 * daisy-chained right group. Script modules render their last frame;
 * dead ones keep it frozen (§6.3). */
static unsigned
mod_w(wm_t *wm, monitor_t *mon, module_t *m)
{
    const mod_reg_t *reg = mod_lookup(m->type);

    if (m->exec)
        return m->flen
            ? draw_text_w(wm, bar_font(wm, m), m->frame,
                (unsigned)m->flen) + 2 * BAR_PAD
            : 0;
    return reg && reg->render
        ? reg->render(wm, mon, m, -10000, false)
        : 0;
}

static void
mod_draw(wm_t *wm, monitor_t *mon, module_t *m, int x)
{
    const mod_reg_t *reg = mod_lookup(m->type);

    if (m->exec) {
        font_t *f = bar_font(wm, m);

        draw_text(wm, &mon->bar->draw, f, x + (int)BAR_PAD,
            bar_baseline(wm, mon, f), m->frame, (unsigned)m->flen,
            bar_color(m, cfg.bar_fg), cfg.bar_bg);
    } else if (reg && reg->render) {
        reg->render(wm, mon, m, x, true);
    }
}

void
bar_render(wm_t *wm, monitor_t *mon)
{
    bar_t *b = mon->bar;

    if (!b || b->win == XCB_NONE)
        return;
    int bw = (int)mon->geom.w - 2 * (int)cfg.bar_gap;

    draw_rect(wm, &b->draw, 0, 0, (unsigned)bw, b->height, cfg.bar_bg);
    bar_draw_logo(wm, mon);

    int x = LOGO_W + 2 * LOGO_PAD;
    bool first = true;

    for (unsigned i = 0; i < b->nleft; i++) {
        module_t *m = &b->mods[i];
        unsigned w = mod_w(wm, mon, m);

        if (!w)
            continue;
        if (!first)
            x += (int)BAR_PAD;
        mod_draw(wm, mon, m, x);
        x += (int)w + (int)BAR_PAD;
        first = false;
    }

    /* center group: measure, then place centered in the bar */
    unsigned total = 0;
    unsigned widths[BAR_MAX_MODULES] = { 0 };

    for (unsigned i = b->nleft; i < b->nleft + b->ncenter; i++) {
        unsigned w = mod_w(wm, mon, &b->mods[i]);

        widths[i] = w;
        total += w ? w + BAR_PAD : 0;
    }
    if (total) {
        x = (bw - (int)total + (int)BAR_PAD) / 2;
        first = true;
        for (unsigned i = b->nleft; i < b->nleft + b->ncenter; i++) {
            if (!widths[i])
                continue;
            if (!first)
                x += (int)BAR_PAD;
            mod_draw(wm, mon, &b->mods[i], x);
            x += (int)widths[i] + (int)BAR_PAD;
            first = false;
        }
    }

    /* right group: measure everything, then place from the edge. */
    unsigned rw[BAR_MAX_MODULES] = { 0 };
    unsigned rtotal = 0;

    for (unsigned i = b->nleft + b->ncenter; i < b->nmods; i++) {
        unsigned w = mod_w(wm, mon, &b->mods[i]);

        rw[i] = w;
        rtotal += w ? w + BAR_PAD : 0;
    }

    font_t *df = draw_ui_font(wm);
    unsigned sepw = draw_text_w(wm, df, "|", 1);
    int rx = bw - (int)rtotal - (int)BAR_PAD;

    /* positions first, then separators centered in each gap */
    int xs[BAR_MAX_MODULES] = { 0 };
    unsigned prev_end = 0;
    bool have_prev = false;

    for (unsigned i = b->nleft + b->ncenter; i < b->nmods; i++) {
        if (!rw[i])
            continue;
        xs[i] = rx;
        if (have_prev) {
            int gap_l = (int)prev_end + (int)BAR_PAD;
            int gap_r = rx + (int)BAR_PAD;
            int sx = (gap_l + gap_r) / 2 - (int)sepw / 2;

            draw_text(wm, &b->draw, df, sx, bar_baseline(wm, mon, df),
                "|", 1, BAR_DIM, cfg.bar_bg);
        }
        prev_end = rx + (int)rw[i] - (int)BAR_PAD;
        have_prev = true;
        rx += (int)rw[i] + (int)BAR_PAD;
    }

    for (unsigned i = b->nleft + b->ncenter; i < b->nmods; i++)
        if (rw[i])
            mod_draw(wm, mon, &b->mods[i], xs[i]);
}

void
bar_render_all(wm_t *wm)
{
    for (monitor_t *m = wm->mons; m; m = m->next)
        bar_render(wm, m);
}

/* A bar window was (re)mapped or uncovered: the server repaints it
 * with the window's background pixel, discarding whatever was drawn
 * before mapping, so an Expose must redraw. Also re-raise the bar so a
 * compositor or tiled client can't sit on top of the thin strip. */
void
bar_expose(wm_t *wm, xcb_window_t win)
{
    for (monitor_t *m = wm->mons; m; m = m->next)
        if (m->bar && m->bar->win == win) {
            raise_window(wm, win);
            bar_render(wm, m);
            return;
        }
}

/* Milliseconds until the bar needs a refresh: the clock's minute
 * boundary, or a 1 s tick when cpu/ram are present (they delta /proc
 * between reads). -1 when nothing is time-driven. */
int
bar_timeout_ms(wm_t *wm)
{
    time_t now = time(NULL);
    int tick = -1;
    bool stats = false;

    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar)
            continue;
        for (unsigned i = 0; i < m->bar->nmods; i++) {
            const char *type = m->bar->mods[i].type;

            if (!type)
                continue;
            if (!strcmp(type, "clock"))
                tick = (int)((60 - now % 60) * 1000) + 250;
            else if (!strcmp(type, "cpu") || !strcmp(type, "ram"))
                stats = true;
        }
    }
    if (stats)
        tick = tick < 0 || 1000 < tick ? 1000 : tick;
    return tick;
}

bool
bar_button(wm_t *wm, xcb_window_t win, int px, unsigned btn)
{
    for (monitor_t *m = wm->mons; m; m = m->next) {
        if (!m->bar || m->bar->win != win)
            continue;
        /* logo hit zone sits ahead of every module */
        if (px >= 0 && px < LOGO_W + 2 * LOGO_PAD &&
            btn == XCB_BUTTON_INDEX_1) {
            menu_apps_open(wm);
            return true;
        }

        /* hit-testing walks all three groups with the same geometry
         * as bar_render, or clicks drift from their frames (they did:
         * the leading BAR_PAD + first-module skip were once missing). */
        bar_t *b = m->bar;
        int bw = (int)m->geom.w - 2 * (int)cfg.bar_gap;
        int x = LOGO_W + 2 * LOGO_PAD;
        bool first = true;

        for (unsigned i = 0; i < b->nleft; i++) {
            module_t *mod = &b->mods[i];
            const mod_reg_t *reg = mod_lookup(mod->type);
            unsigned w = mod_w(wm, m, mod);

            if (!w)
                continue;
            if (!first)
                x += (int)BAR_PAD;
            if (px >= x && px < x + (int)(w + BAR_PAD)) {
                if (btn == XCB_BUTTON_INDEX_3)
                    menu_open(wm); /* §6.4: right-click = settings */
                else if (reg && reg->click)
                    reg->click(wm, m, mod, x, px - x, btn);
                return true;
            }
            x += (int)w + (int)BAR_PAD;
            first = false;
        }

        /* center group */
        unsigned total = 0;
        unsigned widths[BAR_MAX_MODULES] = { 0 };

        for (unsigned i = b->nleft; i < b->nleft + b->ncenter; i++) {
            unsigned w = mod_w(wm, m, &b->mods[i]);

            widths[i] = w;
            total += w ? w + BAR_PAD : 0;
        }
        if (total) {
            x = (bw - (int)total + (int)BAR_PAD) / 2;
            first = true;
            for (unsigned i = b->nleft; i < b->nleft + b->ncenter; i++) {
                module_t *mod = &b->mods[i];
                const mod_reg_t *reg = mod_lookup(mod->type);

                if (!widths[i])
                    continue;
                if (!first)
                    x += (int)BAR_PAD;
                if (px >= x && px < x + (int)(widths[i] + BAR_PAD)) {
                    if (btn == XCB_BUTTON_INDEX_3)
                        menu_open(wm);
                    else if (reg && reg->click)
                        reg->click(wm, m, mod, x, px - x, btn);
                    return true;
                }
                x += (int)widths[i] + (int)BAR_PAD;
                first = false;
            }
        }

        /* right group, placed from the right edge */
        unsigned rw[BAR_MAX_MODULES] = { 0 };
        unsigned rtotal = 0;

        for (unsigned i = b->nleft + b->ncenter; i < b->nmods; i++) {
            unsigned w = mod_w(wm, m, &b->mods[i]);

            rw[i] = w;
            rtotal += w ? w + BAR_PAD : 0;
        }
        x = bw - (int)rtotal - (int)BAR_PAD;
        first = true;
        for (unsigned i = b->nleft + b->ncenter; i < b->nmods; i++) {
            module_t *mod = &b->mods[i];
            const mod_reg_t *reg = mod_lookup(mod->type);

            if (!rw[i])
                continue;
            if (!first)
                x += (int)BAR_PAD;
            if (px >= x && px < x + (int)(rw[i] + BAR_PAD)) {
                if (btn == XCB_BUTTON_INDEX_3)
                    menu_open(wm);
                else if (reg && reg->click)
                    reg->click(wm, m, mod, x, px - x, btn);
                return true;
            }
            x += (int)rw[i] + (int)BAR_PAD;
            first = false;
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
    if (m->bar->win != XCB_NONE) {
        xcb_free_gc(wm->conn, m->bar->draw.gc);
        xcb_destroy_window(wm->conn, m->bar->win);
    }
    free(m->bar);
    m->bar = NULL;
}

void
bars_shutdown(wm_t *wm)
{
    for (monitor_t *m = wm->mons; m; m = m->next)
        bar_teardown(wm, m);
}

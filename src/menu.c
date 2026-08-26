#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actions.h"
#include "bar.h"
#include "client.h"
#include "conf.h"
#include "draw.h"
#include "keys.h"
#include "layout.h"
#include "apps.h"
#include "menu.h"
#include "monitor.h"
#include "popup.h"
#include "settings.h"
#include "util.h"
#include "workspace.h"

#include <xcb/xcb_keysyms.h>

#define ROW_H_MIN 16
#define MENU_W_FRAC 2 /* half the monitor: two columns */
#define MAX_ROWS 96

static unsigned row_h_cache;

static unsigned
row_h(void)
{
    return row_h_cache > ROW_H_MIN ? row_h_cache : ROW_H_MIN;
}

/* Row metrics depend on the UI font; recompute whenever a menu or
 * panel opens so conf font changes apply on next open. */
static void
row_h_update(wm_t *wm)
{
    unsigned h = font_height(draw_ui_font(wm)) + 6;

    row_h_cache = h;
}

typedef enum {
    R_HDR,
    R_BOOL,
    R_ENUM,
    R_INT,
    R_DBL,
    R_STR,
    R_COLOR,
    R_BIND,
} rtype_t;

typedef struct {
    rtype_t type;
    int col;
    const char *label;
    bool *bval;
    int *ival;
    int istep, ilo, ihi;
    double *dval;
    double dstep;
    char **sval;
    const char *const *enames;
    int (*eget)(void);
    void (*eset)(int);
    bind_t *bind;
} row_t;

static int pos_get(void);
static void pos_set(int);
static int layout_get(void);
static void layout_set(int);
static int modmask_get(void);
static void modmask_set(int);

static xcb_window_t win;
static unsigned cur_w, cur_h; /* last applied panel geometry */
static int cur_x, cur_y;
static draw_t draw;
static bool is_open;
static row_t rows[MAX_ROWS];
static unsigned nrows;
static int sel;
static int edit_mode; /* 0 nav, 1 text edit, 2 bind capture */
static char editbuf[128];

static const char *const pos_names[] = { "top", "bottom", NULL };
static const char *const layout_names[] = { "tile", "monocle", NULL };
static const char *const mod_names[] = { "alt", "super", NULL };
static const char *const scope_names[] = { "all", "monitor", NULL };

static int scope_get(void);
static void scope_set(int);

/* ---- enum backends -------------------------------------------------- */

static int scope_get(void)
{
    return cfg.switcher_monitor_scope ? 1 : 0;
}

static void scope_set(int i)
{
    cfg.switcher_monitor_scope = i == 1;
}


static int pos_get(void)
{
    return cfg.bar_bottom ? 1 : 0;
}

static void pos_set(int i)
{
    cfg.bar_bottom = i != 0;
}

static int layout_get(void)
{
    return strcmp(cfg.default_layout, "monocle") == 0 ? 1 : 0;
}

static void layout_set(int i)
{
    free(cfg.default_layout);
    cfg.default_layout = xstrdup(i ? "monocle" : "tile");
}

static int modmask_get(void)
{
    return cfg.mouse_mods == XCB_MOD_MASK_1 ? 0 : 1;
}

static void modmask_set(int i)
{
    cfg.mouse_mods = i ? XCB_MOD_MASK_4 : XCB_MOD_MASK_1;
}

/* ---- row generation: one row per §9.4 field ----------------------- */

static row_t *
add_row(rtype_t t, int col, const char *label)
{
    if (nrows >= MAX_ROWS)
        return NULL;
    row_t *r = &rows[nrows++];

    memset(r, 0, sizeof(*r));
    r->type = t;
    r->col = col;
    r->label = label;
    return r;
}

static void
gen_rows(void)
{
    nrows = 0;
    memset(rows, 0, sizeof(rows));

    /* left column */
    row_t *r = add_row(R_HDR, 0, "general");

    r = add_row(R_STR, 0, "terminal");
    r->sval = &cfg.terminal;
    r = add_row(R_BOOL, 0, "socket");
    r->bval = &cfg.socket;

    add_row(R_HDR, 0, "appearance");
    r = add_row(R_INT, 0, "border_width");
    r->ival = (int *)&cfg.border_width;
    r->istep = 1;
    r->ilo = 0;
    r->ihi = 32;
    r = add_row(R_COLOR, 0, "focus_color");
    r->sval = (char **)&cfg.focus_color;
    r = add_row(R_COLOR, 0, "unfocus_color");
    r->sval = (char **)&cfg.unfocus_color;
    r = add_row(R_COLOR, 0, "urgent_color");
    r->sval = (char **)&cfg.urgent_color;
    r = add_row(R_INT, 0, "gap");
    r->ival = (int *)&cfg.gap;
    r->istep = 1;
    r->ilo = 0;
    r->ihi = 128;
    r = add_row(R_BOOL, 0, "smart_gaps");
    r->bval = &cfg.smart_gaps;
    r = add_row(R_INT, 0, "corner_radius");
    r->ival = (int *)&cfg.corner_radius;
    r->istep = 1;
    r->ilo = 0;
    r->ihi = 64;
    r = add_row(R_STR, 0, "font");
    r->sval = &cfg.font;

    add_row(R_HDR, 0, "bar");
    r = add_row(R_ENUM, 0, "position");
    r->enames = pos_names;
    r->eget = pos_get;
    r->eset = pos_set;
    r = add_row(R_STR, 0, "time_format");
    r->sval = &cfg.time_format;
    r = add_row(R_COLOR, 0, "bar_bg");
    r->sval = (char **)&cfg.bar_bg;
    r = add_row(R_COLOR, 0, "bar_fg");
    r->sval = (char **)&cfg.bar_fg;
    r = add_row(R_INT, 0, "bar_gap");
    r->ival = (int *)&cfg.bar_gap;

    add_row(R_HDR, 0, "behavior");
    r = add_row(R_BOOL, 0, "focus_follows_mouse");
    r->bval = &cfg.focus_follows_mouse;
    r = add_row(R_BOOL, 0, "raise_on_click");
    r->bval = &cfg.raise_on_click;
    r = add_row(R_INT, 0, "snap_distance");
    r->ival = (int *)&cfg.snap_distance;
    r->istep = 1;
    r->ilo = 0;
    r->ihi = 256;
    r = add_row(R_INT, 0, "popup_timeout");
    r->ival = (int *)&cfg.popup_timeout;
    r->istep = 1;
    r->ilo = 1;
    r->ihi = 60;
    r = add_row(R_BOOL, 0, "swallowing");
    r->bval = &cfg.swallowing;

    add_row(R_HDR, 0, "layouts");
    r = add_row(R_ENUM, 0, "default");
    r->enames = layout_names;
    r->eget = layout_get;
    r->eset = layout_set;
    r = add_row(R_INT, 0, "nmaster");
    r->ival = (int *)&cfg.nmaster;
    r->istep = 1;
    r->ilo = 1;
    r->ihi = WS_MAX;
    r = add_row(R_DBL, 0, "split_ratio");
    r->dval = &cfg.split_ratio;
    r->dstep = 0.05;
    r = add_row(R_DBL, 0, "ratio_step");
    r->dval = &cfg.ratio_step;
    r->dstep = 0.01;

    add_row(R_HDR, 0, "wallpaper");
    r = add_row(R_STR, 0, "setter_command");
    r->sval = &cfg.wp_setter;
    r = add_row(R_INT, 0, "wallpaper_dirs");
    r->ival = (int *)&cfg.nwp_dirs;
    r->istep = 0;
    r->ilo = 0;
    r->ihi = 0;

    add_row(R_HDR, 0, "panels");
    r = add_row(R_ENUM, 0, "switcher_scope");
    r->enames = scope_names;
    r->eget = scope_get;
    r->eset = scope_set;
    r = add_row(R_BOOL, 0, "launcher_scan_path");
    r->bval = &cfg.launcher_scan_path;
    r = add_row(R_STR, 0, "launcher_custom_dir");
    r->sval = &cfg.launcher_custom_dir;
    r = add_row(R_INT, 0, "launcher_history_size");
    r->ival = (int *)&cfg.launcher_history_size;
    r->istep = 1;
    r->ilo = 0;
    r->ihi = 500;
    r = add_row(R_STR, 0, "launcher_default_module");
    r->sval = &cfg.launcher_default_module;

    add_row(R_HDR, 0, "mouse");
    r = add_row(R_ENUM, 0, "modifier");
    r->enames = mod_names;
    r->eget = modmask_get;
    r->eset = modmask_set;
    r = add_row(R_INT, 0, "move_button");
    r->ival = (int *)&cfg.move_button;
    r->istep = 1;
    r->ilo = 1;
    r->ihi = 5;
    r = add_row(R_INT, 0, "resize_button");
    r->ival = (int *)&cfg.resize_button;
    r->istep = 1;
    r->ilo = 1;
    r->ihi = 5;

    /* right column */
    add_row(R_HDR, 1, "workspaces");
    for (unsigned i = 0; i < WS_MAX; i++) {
        static char labels[WS_MAX][12];

        snprintf(labels[i], sizeof(labels[i]), "name %u", i + 1);
        r = add_row(R_STR, 1, labels[i]);
        r->sval = &cfg.ws_names[i];
    }

    add_row(R_HDR, 1, "keys");
    for (unsigned i = 0; i < cfg.nbinds && nrows < MAX_ROWS; i++) {
        r = add_row(R_BIND, 1, "bind");
        r->bind = &cfg.binds[i];
    }
}

/* ---- value formatting --------------------------------------------- */

static void
row_value(const row_t *r, char *out, unsigned outsz)
{
    switch (r->type) {
    case R_BOOL:
        snprintf(out, outsz, "%s", *r->bval ? "on" : "off");
        break;
    case R_ENUM:
        snprintf(out, outsz, "%s", r->enames[r->eget()]);
        break;
    case R_INT:
        snprintf(out, outsz, "%d", *r->ival);
        break;
    case R_DBL:
        snprintf(out, outsz, "%.2f", *r->dval);
        break;
    case R_STR:
        snprintf(out, outsz, "%s", *r->sval ? *r->sval : "");
        break;
    case R_COLOR: {
        uint32_t v = *(uint32_t *)r->sval;

        snprintf(out, outsz, "#%06x", v);
        break;
    }
    case R_BIND: {
        char combo[64] = "";

        if (r->bind->mods & XCB_MOD_MASK_4)
            strcat(combo, "super+");
        if (r->bind->mods & XCB_MOD_MASK_CONTROL)
            strcat(combo, "ctrl+");
        if (r->bind->mods & XCB_MOD_MASK_1)
            strcat(combo, "alt+");
        if (r->bind->mods & XCB_MOD_MASK_SHIFT)
            strcat(combo, "shift+");
        keysym_name(r->bind->keysym, combo + strlen(combo),
            (unsigned)(sizeof(combo) - strlen(combo)));
        uint8_t id = r->bind->action;

        if (id == ACT_VIEW_WS || id == ACT_SEND_WS)
            snprintf(out, outsz, "%s %s %d", combo,
                action_name(id), r->bind->arg);
        else
            snprintf(out, outsz, "%s %s", combo, action_name(id));
        break;
    }
    default:
        out[0] = '\0';
    }
}

/* ---- drawing ------------------------------------------------------- */

static unsigned
menu_height(void)
{
    unsigned left = 0, right = 0;

    for (unsigned i = 0; i < nrows; i++)
        rows[i].col ? right++ : left++;
    unsigned m = left > right ? left : right;

    return m * row_h() + 3 * row_h(); /* title + rows + footer */
}

static bool panel_mode;
static void panel_draw(wm_t *wm);
static void
menu_draw(wm_t *wm)
{
    if (panel_mode) {
        panel_draw(wm);
        return;
    }
    monitor_t *mon = focused_mon(wm);
    unsigned w = mon->geom.w / 2;
    unsigned h = menu_height();
    font_t *f = draw_ui_font(wm);

    draw_rect(wm, &draw, 0, 0, w, h, 0x181818);
    draw_rect(wm, &draw, 0, 0, w, row_h(), 0x242424);
    draw_text(wm, &draw, f, 6, row_h() - 5, "austere settings", 15,
        0xdddddd, 0x242424);

    unsigned col_x[2] = { 6, w / 2 + 6 };
    unsigned col_w[2] = { w / 2 - 12, w / 2 - 12 };
    unsigned y[2] = { row_h() + 2, row_h() + 2 };

    for (unsigned i = 0; i < nrows; i++) {
        row_t *r = &rows[i];
        int cx = (int)col_x[r->col];
        unsigned cwid = col_w[r->col];
        int by = (int)y[r->col];

        if (r->type == R_HDR) {
            draw_text(wm, &draw, f, cx, by + row_h() - 5, r->label,
                (unsigned)strlen(r->label), 0x888888, 0x181818);
            y[r->col] += row_h();
            continue;
        }
        bool is_sel = (int)i == sel;

        if (is_sel)
            draw_rect(wm, &draw, cx - 3, by, cwid, row_h(), 0x2c3e50);
        uint32_t fg = is_sel ? 0xffffff : 0xbbbbbb;
        uint32_t rowbg = is_sel ? 0x2c3e50 : 0x181818;

        draw_text(wm, &draw, f, cx, by + row_h() - 5, r->label,
            (unsigned)strlen(r->label), fg, rowbg);
        char val[128];
        row_value(r, val, sizeof(val));
        unsigned vw = draw_text_w(wm, f, val, (unsigned)strlen(val));
        draw_text(wm, &draw, f, (int)(cx + cwid - vw), by + row_h() - 5,
            val, (unsigned)strlen(val),
            r->type == R_BIND ? 0x9fb8c8 : fg, rowbg);
        y[r->col] += row_h();
    }

    /* footer / prompt */
    const char *hint = edit_mode == 2
        ? "press new combo  (Esc cancels)"
        : edit_mode == 1
        ? "value: _  (Return accepts, Esc cancels)"
        : "arrows adjust - Tab column - Ctrl+s save - Esc close";
    unsigned fh = h - row_h() + 4;

    draw_rect(wm, &draw, 0, (int)(h - row_h()), w, row_h(), 0x242424);
    draw_text(wm, &draw, f, 6, (int)(fh + 2), hint,
        (unsigned)strlen(hint), edit_mode ? 0xffcc66 : 0x888888,
        0x242424);
    if (edit_mode == 1)
        draw_text(wm, &draw, f, 60, (int)(fh + 2), editbuf,
            (unsigned)strlen(editbuf), 0xffffff, 0x242424);
}

/* ---- application menu ------------------------------------------------ */

static int apps_pending_cat = -1;

static void apps_open_cat(wm_t *wm, unsigned cat);

static bool
apps_l2_enter(wm_t *wm, const char *input, const char *row)
{
    (void)input;
    const char *exec = row ? app_exec_for(row) : NULL;

    if (exec) {
        spawn_shell(exec);
        popup_notify(wm, "%s", row);
    }
    return false;
}

static bool
apps_cat_enter(wm_t *wm, const char *input, const char *row)
{
    (void)wm;
    (void)input;
    for (unsigned c = 0; c < 5; c++)
        if (row && !strcmp(row, app_category_name(c))) {
            apps_pending_cat = (int)c;
            return false; /* close; on_close drills in */
        }
    return false;
}

static void
apps_l1_close(wm_t *wm)
{
    if (apps_pending_cat < 0)
        return;
    int c = apps_pending_cat;

    apps_pending_cat = -1;
    apps_open_cat(wm, (unsigned)c);
}

static void
apps_open_cat(wm_t *wm, unsigned cat)
{
    static char **rows;
    static unsigned n;

    free(rows);
    rows = NULL;
    n = 0;
    for (unsigned i = 0; i < apps_count(); i++)
        if (app_category(i) == (int)cat)
            n++;
    if (!n)
        return;
    rows = xmalloc(n * sizeof(char *));
    unsigned k = 0;

    for (unsigned i = 0; i < apps_count(); i++)
        if (app_category(i) == (int)cat)
            rows[k++] = xstrdup(app_name(i));
    for (unsigned a = 0; a + 1 < n; a++)
        for (unsigned b = a + 1; b < n; b++)
            if (strcasecmp(rows[a], rows[b]) > 0) {
                char *t = rows[a];

                rows[a] = rows[b];
                rows[b] = t;
            }
    panel_def_t def = {
        .title = app_category_name(cat), .prompt = "", .rows = rows,
        .nrows = n, .filter = true, .on_enter = apps_l2_enter,
        .px_w = 320, .anchor_bar = true
    };

    panel_open(wm, &def);
}

void
menu_apps_open(wm_t *wm)
{
    apps_rescan();
    if (!apps_count()) {
        popup_notify(wm, "no applications found");
        return;
    }
    unsigned per[5] = { 0 };

    for (unsigned i = 0; i < apps_count(); i++) {
        int c = app_category(i);

        if (c >= 0 && c < 5)
            per[c]++;
    }
    static char *cats[5];

    for (unsigned c = 0; c < 5; c++)
        cats[c] = (char *)app_category_name(c);
    unsigned shown = 0, only = 5;

    for (unsigned c = 0; c < 5; c++)
        if (per[c]) {
            shown++;
            only = c;
        }
    if (shown == 1) {
        apps_open_cat(wm, only);
        return;
    }
    panel_def_t def = {
        .title = "applications", .prompt = "",
        .rows = cats, .nrows = 5, .filter = false,
        .on_enter = apps_cat_enter, .on_close = apps_l1_close,
        .px_w = 320, .anchor_bar = true
    };

    panel_open(wm, &def);
}

/* ---- shared list panel (switcher/launcher/welcome) ------------------ */

static panel_def_t pdef;
static char pinput[256];
static overlay_t overlay;
static bool overlay_active;
static char **pview; /* filtered row indices */
static unsigned pview_n;
static unsigned psel;
static char ptitle[64];
static char pprompt[64];

static bool
ncasestr(const char *hay, const char *needle)
{
    if (!*needle)
        return true;
    size_t nl = strlen(needle);

    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0)
            return true;
    return false;
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Prefix matches rank before substring matches; each group is
 * alphabetized so the top row is deterministic. */
static void
panel_refilter(void)
{
    free(pview);
    pview = pdef.nrows
        ? xmalloc(pdef.nrows * sizeof(char *))
        : NULL;
    pview_n = 0;
    if (!pinput[0] || !pdef.filter) {
        /* keep the opener's curated order (history first) */
        for (unsigned i = 0; i < pdef.nrows; i++)
            pview[pview_n++] = pdef.rows[i];
    } else {
        size_t plen = strlen(pinput);

        for (unsigned i = 0; i < pdef.nrows; i++)
            if (strncasecmp(pdef.rows[i], pinput, plen) == 0)
                pview[pview_n++] = pdef.rows[i];
        unsigned nprefix = pview_n;

        for (unsigned i = 0; i < pdef.nrows; i++)
            if (ncasestr(pdef.rows[i], pinput) &&
                strncasecmp(pdef.rows[i], pinput, plen) != 0)
                pview[pview_n++] = pdef.rows[i];
        qsort(pview, nprefix, sizeof(char *), cmp_str);
        qsort(pview + nprefix, pview_n - nprefix, sizeof(char *),
            cmp_str);
    }
    if (psel >= pview_n)
        psel = pview_n ? pview_n - 1 : 0;
}

const char *
panel_input(void)
{
    return pinput;
}

void
panel_set_input(const char *s)
{
    snprintf(pinput, sizeof(pinput), "%s", s);
    panel_refilter();
    psel = 0;
}

/* Panel placement. Default: centered, sized to content. Bar-anchored
 * menus (application menu): skinny column aligned with the bar's left
 * edge, filling the workarea — workarea already sits below a top bar /
 * above a bottom one, so one formula serves both. */
static void
panel_layout(wm_t *wm, int *rx, int *ry, unsigned *rw, unsigned *rh,
    unsigned *nvis_out)
{
    monitor_t *mon = focused_mon(wm);
    Rect a = mon_workarea(mon);
    unsigned w, h;
    int x, y;

    if (pdef.anchor_bar) {
        /* share the tiled grid's outer insets so the menu's top edge
         * lines up with the first client row */
        int g = (int)cfg.gap;
        int aw = (int)a.w - 2 * g;
        int ah = (int)a.h - 2 * g;

        if (aw < 160)
            aw = (int)a.w;
        if (ah < 3 * (int)row_h())
            ah = (int)a.h;
        w = (unsigned)aw < 320 ? (unsigned)aw : 320;
        h = (unsigned)ah;
        x = a.x + g;
        y = a.y + g;
    } else {
        w = mon->geom.w * 3 / 5;
        h = (pview_n < (a.h - 4 * row_h()) / row_h()
                ? pview_n : (a.h - 4 * row_h()) / row_h())
            * row_h() + 3 * row_h();
        x = a.x + (int)((a.w - w) / 2);
        y = a.y + (int)((a.h - h) / 2);
    }
    unsigned max_vis = (h - 3 * row_h()) / row_h();
    unsigned nvis = pview_n < max_vis ? pview_n : max_vis;

    if (!pdef.anchor_bar)
        h = nvis * row_h() + 3 * row_h();
    *rx = x;
    *ry = y;
    *rw = w;
    *rh = h;
    *nvis_out = nvis;
}

static void
panel_draw(wm_t *wm)
{
    int px, py;
    unsigned w, h, nvis;

    panel_layout(wm, &px, &py, &w, &h, &nvis);
    font_t *f = draw_ui_font(wm);
    unsigned base = psel >= nvis ? psel - nvis + 1 : 0;

    if (w != cur_w || h != cur_h || px != cur_x || py != cur_y) {
        uint32_t vals[] = { (uint32_t)px, (uint32_t)py, w, h };

        xcb_configure_window(wm->conn, win,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            vals);
        cur_w = w;
        cur_h = h;
        cur_x = px;
        cur_y = py;
    }
    draw_rect(wm, &draw, 0, 0, w, h, 0x181818);
    draw_rect(wm, &draw, 0, 0, w, row_h(), 0x242424);
    draw_text(wm, &draw, f, 6, row_h() - 5, ptitle,
        (unsigned)strlen(ptitle), 0xdddddd, 0x242424);

    for (unsigned i = 0; i < nvis; i++) {
        const char *row = pview[base + i];
        bool selected = base + i == psel;

        if (selected)
            draw_rect(wm, &draw, 3, (int)(row_h() + i * row_h()),
                w - 6, row_h(), 0x2c3e50);
        draw_text(wm, &draw, f, 6, (int)(row_h() + i * row_h() + row_h() - 5),
            row, (unsigned)strlen(row),
            selected ? 0xffffff : 0xbbbbbb,
            selected ? 0x2c3e50 : 0x181818);
    }

    /* prompt line with input + optional module hint */
    char line[256];

    int pl = snprintf(line, sizeof(line), "%s", pprompt);

    if (pl < 0)
        pl = 0;
    snprintf(line + pl, sizeof(line) - (size_t)pl, "%s", pinput);
    draw_rect(wm, &draw, 0, (int)(h - 2 * row_h()), w, row_h(), 0x242424);
    draw_text(wm, &draw, f, 6, (int)(h - 2 * row_h() + row_h() - 5), line,
        (unsigned)strlen(line), 0xffffff, 0x242424);

    draw_rect(wm, &draw, 0, (int)(h - row_h()), w, row_h(), 0x1e1e1e);
    const char *foot = "type to filter - Enter run - Tab complete - Esc";
    draw_text(wm, &draw, f, 6, (int)(h - row_h() + 3), foot,
        (unsigned)strlen(foot), 0x777777, 0x1e1e1e);
}

static void
panel_create_window(wm_t *wm)
{
    int px, py;
    unsigned w, h, nvis;

    panel_layout(wm, &px, &py, &w, &h, &nvis);

    win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, win,
        wm->scr->root, (int16_t)px, (int16_t)py,
        (uint16_t)w, (uint16_t)h, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
            XCB_CW_EVENT_MASK,
        (uint32_t[]){ 0x181818, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
                XCB_EVENT_MASK_BUTTON_PRESS });
    draw_setup(wm, &draw, win);
    cur_w = w;
    cur_h = h;
    xcb_map_window(wm->conn, win);
    uint32_t above = XCB_STACK_MODE_ABOVE;

    xcb_configure_window(wm->conn, win, XCB_CONFIG_WINDOW_STACK_MODE,
        &above);
}

static void
panel_finish(wm_t *wm, bool run_enter)
{
    char input[256];
    const char *row = pview_n ? pview[psel] : NULL;

    snprintf(input, sizeof(input), "%s", pinput);
    if (run_enter && pdef.on_enter) {
        if (pdef.on_enter(wm, input, row)) {
            menu_draw(wm); /* held open with a new input */
            return;
        }
    }
    void (*on_close)(wm_t *) = pdef.on_close;

    /* def memory is owned by the opener; clear state before teardown
     * so callbacks can open another panel safely */
    is_open = false;
    panel_mode = false;
    free(pview);
    pview = NULL;
    pview_n = 0;
    pinput[0] = '\0';
    memset(&pdef, 0, sizeof(pdef));
    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    xcb_destroy_window(wm->conn, win);
    win = XCB_NONE;
    if (on_close)
        on_close(wm);
    arrange(wm);
    bar_render_all(wm);
}

/* returns true when the key was consumed as panel text input */
static bool
panel_text_key(wm_t *wm, xcb_keysym_t sym)
{
    size_t len = strlen(pinput);

    if (sym == 0xff08) { /* BackSpace */
        if (len)
            pinput[len - 1] = '\0';
    } else if (sym >= 0x20 && sym <= 0x7e && len + 1 < sizeof(pinput)) {
        pinput[len] = (char)sym;
        pinput[len + 1] = '\0';
    } else {
        return false;
    }
    panel_refilter();
    psel = 0;
    menu_draw(wm); /* panel_draw shares the expose path */
    return true;
}

void
panel_key(wm_t *wm, xcb_key_press_event_t *ev)
{
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(wm->keysyms,
        ev->detail, 0);


    switch (sym) {
    case 0xff1b: /* Escape */
        panel_finish(wm, false);
        return;
    case 0xff0d: /* Return */
        panel_finish(wm, true);
        return;
    case 0xff52: /* Up */
        if (pview_n)
            psel = psel == 0 ? pview_n - 1 : psel - 1;
        break;
    case 0xff54: /* Down */
        if (pview_n)
            psel = psel + 1 == pview_n ? 0 : psel + 1;
        break;
    case 0xff09: /* Tab */
        if (pdef.tab_complete && pinput[0]) {
            /* complete to common prefix of matching rows */
            size_t n = strlen(pinput);

            for (unsigned i = 0; i < pview_n; i++) {
                const char *r = pview[i];

                if (strncasecmp(r, pinput, n) != 0)
                    continue;
                while (r[n] && pinput[0]) {
                    char cand[256];

                    snprintf(cand, sizeof(cand), "%.*s", (int)(n + 1),
                        r);
                    bool all = true;

                    for (unsigned j = 0; j < pview_n; j++)
                        if (strncasecmp(pview[j], cand, n + 1) != 0)
                            all = false;
                    if (!all)
                        break;
                    snprintf(pinput, sizeof(pinput), "%s", cand);
                    n++;
                }
                break;
            }
            panel_refilter();
        }
        break;
    default:
        if (panel_text_key(wm, sym))
            return;
        return;
    }
    menu_draw(wm);
}

/* ---- window lifecycle ---------------------------------------------- */

static void
menu_create_window(wm_t *wm)
{
    monitor_t *mon = focused_mon(wm);
    unsigned w = mon->geom.w / 2;
    unsigned h = menu_height();
    Rect a = mon_workarea(mon);

    win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, win,
        wm->scr->root,
        (int16_t)(a.x + (a.w - w) / 2), (int16_t)(a.y + (a.h - h) / 2),
        (uint16_t)w, (uint16_t)h, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
            XCB_CW_EVENT_MASK,
        (uint32_t[]){ 0x181818, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS });
    draw_setup(wm, &draw, win);
    cur_w = w;
    cur_h = h;
    xcb_map_window(wm->conn, win);
    uint32_t above = XCB_STACK_MODE_ABOVE;

    xcb_configure_window(wm->conn, win, XCB_CONFIG_WINDOW_STACK_MODE,
        &above);
}

void
panel_open(wm_t *wm, const panel_def_t *def)
{
    if (is_open)
        return;
    pdef = *def;
    panel_mode = true;
    pinput[0] = '\0';
    psel = 0;
    snprintf(ptitle, sizeof(ptitle), "%s",
        def->title ? def->title : "");
    snprintf(pprompt, sizeof(pprompt), "%s",
        def->prompt ? def->prompt : "");
    row_h_update(wm);
    panel_refilter();
    panel_create_window(wm);
    xcb_grab_keyboard(wm->conn, 0, win, XCB_CURRENT_TIME,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    is_open = true;
    menu_draw(wm);
}

void
menu_open(wm_t *wm)
{
    if (is_open)
        return;
    gen_rows();
    sel = 1;
    edit_mode = 0;
    editbuf[0] = '\0';
    row_h_update(wm);
    menu_create_window(wm);
    xcb_grab_keyboard(wm->conn, 0, win, XCB_CURRENT_TIME,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    is_open = true;
    menu_draw(wm);
}

void
menu_close(wm_t *wm)
{
    if (!is_open)
        return;
    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    xcb_destroy_window(wm->conn, win);
    win = XCB_NONE;
    is_open = false;
    panel_mode = false;
    arrange(wm);
    bar_render_all(wm);
}

bool
menu_active(void)
{
    return is_open || overlay_active;
}

bool
menu_owns_window(xcb_window_t w)
{
    return (is_open && w == win) ||
        (overlay_active && w == overlay.win);
}

/* Left click selects the row under the cursor; clicking the row that
 * is already selected activates it. Returns true when consumed. */
bool
menu_panel_button(wm_t *wm, xcb_window_t w, int16_t y, uint8_t btn)
{
    if (btn != XCB_BUTTON_INDEX_1 || !is_open || w != win)
        return false;
    int px, py;
    unsigned pw, ph, nvis;

    panel_layout(wm, &px, &py, &pw, &ph, &nvis);
    unsigned base = psel >= nvis ? psel - nvis + 1 : 0;

    if (y < (int)row_h() || y >= (int)(row_h() + nvis * row_h()))
        return true;
    unsigned idx = base +
        (unsigned)(y - (int)row_h()) / row_h();

    if (idx >= pview_n)
        return true;
    if (idx == psel) {
        panel_finish(wm, true);
        return true;
    }
    psel = idx;
    menu_draw(wm);
    return true;
}

bool
menu_overlay_button(wm_t *wm, xcb_window_t win, int16_t x, int16_t y,
    uint8_t btn, xcb_timestamp_t t)
{
    if (!overlay_active || win != overlay.win || !overlay.button)
        return false;
    return overlay.button(wm, x, y, btn, t);
}

void
menu_push_overlay(wm_t *wm, const overlay_t *o)
{
    if (overlay_active || is_open)
        return;
    overlay = *o;
    overlay_active = true;
    xcb_grab_keyboard(wm->conn, 0, o->win, XCB_CURRENT_TIME,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    o->draw(wm);
}

void
menu_pop_overlay(wm_t *wm)
{
    if (!overlay_active)
        return;
    xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
    overlay_active = false;
    if (overlay.close)
        overlay.close(wm);
    arrange(wm);
    bar_render_all(wm);
}

/* ---- live apply ---------------------------------------------------- */

static void
apply_all(wm_t *wm)
{
    ungrab_keys(wm);
    grab_keys(wm);
    for (client_t *c = wm->clients; c; c = c->next) {
        xcb_configure_window(wm->conn, c->win,
            XCB_CONFIG_WINDOW_BORDER_WIDTH,
            (uint32_t[]){ cfg.border_width });
        set_border(wm, c, wm->focused == c ? cfg.focus_color
                                           : cfg.unfocus_color);
    }
    bars_sync(wm);
    arrange(wm);
}

/* ---- editing ------------------------------------------------------- */

static void
begin_edit(const row_t *r)
{
    edit_mode = 1;
    if (r->type == R_INT)
        snprintf(editbuf, sizeof(editbuf), "%d", *r->ival);
    else if (r->type == R_DBL)
        snprintf(editbuf, sizeof(editbuf), "%.2f", *r->dval);
    else if (r->sval && *r->sval)
        snprintf(editbuf, sizeof(editbuf), "%s", *r->sval);
    else
        editbuf[0] = '\0';
}

static void
commit_edit(wm_t *wm, row_t *r)
{
    char *end;

    switch (r->type) {
    case R_INT: {
        long v = strtol(editbuf, &end, 10);

        if (end != editbuf) {
            if (v < r->ilo)
                v = r->ilo;
            if (v > r->ihi)
                v = r->ihi;
            *r->ival = (int)v;
        }
        break;
    }
    case R_DBL: {
        double v = strtod(editbuf, &end);

        if (end != editbuf)
            *r->dval = v;
        break;
    }
    case R_COLOR: {
        uint32_t c;

        if (conf_parse_color(editbuf, &c))
            *(uint32_t *)r->sval = c;
        else
            popup_notify(wm, "bad color (want #rrggbb)");
        break;
    }
    default:
        if (r->sval) {
            free(*r->sval);
            *r->sval = xstrdup(editbuf);
        }
    }
    edit_mode = 0;
    apply_all(wm);
    menu_draw(wm);
}

static void
adjust(wm_t *wm, row_t *r, int dir)
{
    switch (r->type) {
    case R_BOOL:
        *r->bval = !*r->bval;
        break;
    case R_ENUM: {
        int n = 0;

        while (r->enames[n])
            n++;
        r->eset((r->eget() + (dir > 0 ? 1 : n - 1)) % n);
        break;
    }
    case R_INT: {
        int v = *r->ival + dir * r->istep;

        if (v >= r->ilo && v <= r->ihi)
            *r->ival = v;
        break;
    }
    case R_DBL: {
        *r->dval += dir * r->dstep;
        break;
    }
    default:
        return;
    }
    apply_all(wm);
    menu_draw(wm);
}

/* ---- key handling --------------------------------------------------- */

static void
nav(int dir)
{
    int i = sel;

    for (;;) {
        i += dir;
        if (i < 0 || i >= (int)nrows)
            return;
        if (rows[i].col == rows[sel].col) {
            sel = i;
            return;
        }
    }
}

static void
switch_col(void)
{
    int want = rows[sel].col ? 0 : 1;

    for (int i = 0; i < (int)nrows; i++)
        if (rows[i].col == want && rows[i].type != R_HDR) {
            sel = i;
            return;
        }
}

void
menu_key(wm_t *wm, xcb_key_press_event_t *ev)
{
    if (overlay_active) {
        xcb_keysym_t osym = xcb_key_symbols_get_keysym(wm->keysyms,
            ev->detail, 0);

        if (osym == 0xff1b) { /* Escape */
            menu_pop_overlay(wm);
            return;
        }
        if (overlay.key(wm, osym, ev->state))
            overlay.draw(wm);
        return;
    }
    if (panel_mode) {
        panel_key(wm, ev);
        return;
    }
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(wm->keysyms,
        ev->detail, 0);
    unsigned state = ev->state & (XCB_MOD_MASK_SHIFT |
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4);

    /* bind capture: next non-modifier press becomes the combo */
    if (edit_mode == 2) {
        if (sym >= 0xffe1 && sym <= 0xffee)
            return; /* bare modifier */
        if (sym == 0xff1b) { /* Escape */
            edit_mode = 0;
            menu_draw(wm);
            return;
        }
        row_t *r = &rows[sel];

        r->bind->mods = state;
        r->bind->keysym = sym;
        edit_mode = 0;
        apply_all(wm);
        menu_draw(wm);
        return;
    }

    /* text edit */
    if (edit_mode == 1) {
        size_t len = strlen(editbuf);

        if (sym == 0xff1b) { /* Escape */
            edit_mode = 0;
        } else if (sym == 0xff0d) { /* Return */
            commit_edit(wm, &rows[sel]);
            return;
        } else if (sym == 0xff08) { /* BackSpace */
            if (len)
                editbuf[len - 1] = '\0';
        } else if (sym >= 0x20 && sym <= 0x7e &&
            len + 1 < sizeof(editbuf)) {
            editbuf[len] = (char)sym;
            editbuf[len + 1] = '\0';
        } else {
            return;
        }
        menu_draw(wm);
        return;
    }

    /* navigation */
    if (state & XCB_MOD_MASK_CONTROL && sym == 's') {
        if (conf_write(conf_path(), &cfg))
            popup_notify(wm, "settings saved");
        else
            popup_notify(wm, "save failed");
        return;
    }
    switch (sym) {
    case 0xff1b: /* Escape */
        menu_close(wm);
        return;
    case 0xff52: /* Up */
    case 'k':
        nav(-1);
        break;
    case 0xff54: /* Down */
    case 'j':
        nav(1);
        break;
    case 0xff09: /* Tab */
        switch_col();
        break;
    case 0xff51: /* Left */
        adjust(wm, &rows[sel], -1);
        break;
    case 0xff53: /* Right */
        adjust(wm, &rows[sel], 1);
        break;
    case 0xff0d: /* Return */
        switch (rows[sel].type) {
        case R_BOOL:
            adjust(wm, &rows[sel], 1);
            break;
        case R_ENUM:
            adjust(wm, &rows[sel], 1);
            break;
        case R_BIND:
            edit_mode = 2;
            menu_draw(wm);
            break;
        case R_INT:
        case R_DBL:
        case R_STR:
        case R_COLOR:
            begin_edit(&rows[sel]);
            menu_draw(wm);
            break;
        default:
            break;
        }
        break;
    default:
        return;
    }
    menu_draw(wm);
}

void
menu_expose(wm_t *wm)
{
    if (overlay_active) {
        overlay.draw(wm);
        return;
    }
    if (is_open)
        menu_draw(wm);
}

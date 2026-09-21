#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actions.h"
#include "bar.h"
#include "client.h"
#include "conf.h"
#include "deco.h"
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
    double dstep, dlo, dhi;
    char **sval;
    uint32_t *uval;
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
static const char *const layout_names[] = { "tile", "monocle", "float",
    NULL };
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
    for (unsigned i = 0; layout_names[i]; i++)
        if (!strcmp(cfg.default_layout, layout_names[i]))
            return (int)i;
    return 0;
}

static void layout_set(int i)
{
    free(cfg.default_layout);
    cfg.default_layout = xstrdup(layout_names[i]);
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
    r->uval = &cfg.focus_color;
    r = add_row(R_COLOR, 0, "unfocus_color");
    r->uval = &cfg.unfocus_color;
    r = add_row(R_COLOR, 0, "urgent_color");
    r->uval = &cfg.urgent_color;
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

    add_row(R_HDR, 0, "deco");
    r = add_row(R_BOOL, 0, "deco");
    r->bval = &cfg.deco;
    r = add_row(R_INT, 0, "deco_title_h");
    r->ival = (int *)&cfg.deco_title_h;
    r->istep = 1;
    r->ilo = 10;
    r->ihi = 40;
    r = add_row(R_COLOR, 0, "deco_border");
    r->uval = &cfg.deco_border;
    r = add_row(R_COLOR, 0, "deco_unfocus_border");
    r->uval = &cfg.deco_unfocus_border;

    add_row(R_HDR, 0, "bar");
    r = add_row(R_ENUM, 0, "position");
    r->enames = pos_names;
    r->eget = pos_get;
    r->eset = pos_set;
    r = add_row(R_STR, 0, "time_format");
    r->sval = &cfg.time_format;
    r = add_row(R_COLOR, 0, "bar_bg");
    r->uval = &cfg.bar_bg;
    r = add_row(R_COLOR, 0, "bar_fg");
    r->uval = &cfg.bar_fg;
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
    r->dlo = 0.1;
    r->dhi = 0.9;
    r = add_row(R_DBL, 0, "ratio_step");
    r->dval = &cfg.ratio_step;
    r->dstep = 0.01;
    r->dlo = 0.01;
    r->dhi = 0.5;

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
        uint32_t v = *r->uval;

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
        else if (id == ACT_EXEC)
            snprintf(out, outsz, "%s %s %s", combo,
                action_name(id), r->bind->cmd);
        else
            snprintf(out, outsz, "%s %s", combo, action_name(id));
        break;
    }
    default:
        out[0] = '\0';
    }
}

/* ---- drawing ------------------------------------------------------- */

/* Natural height capped to the focused workarea; nvis is how many
 * rows fit. Both columns scroll in lockstep via the base offset, so
 * tall configs stay usable on short screens. */
static void
menu_metrics(wm_t *wm, unsigned *h, unsigned *nvis)
{
    unsigned left = 0, right = 0;

    for (unsigned i = 0; i < nrows; i++)
        rows[i].col ? right++ : left++;
    unsigned m = left > right ? left : right;
    unsigned natural = m * row_h() + 2 * row_h(); /* title + rows */
    monitor_t *mon = focused_mon(wm);
    Rect a = mon_workarea(mon);
    unsigned top = 2 * row_h();

    *h = natural < a.h ? natural : a.h;
    if (*h < top)
        *h = top;
    *nvis = (*h - top) / row_h();
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
    unsigned h, nvis;

    menu_metrics(wm, &h, &nvis);
    font_t *f = draw_ui_font(wm);

    uint32_t bg = cfg.bar_bg;
    uint32_t fg = cfg.bar_fg;
    uint32_t ac = cfg.focus_color;

    draw_rect(wm, &draw, 0, 0, w, h, bg);
    draw_rect(wm, &draw, 0, 0, w, row_h(), bg);
    draw_text(wm, &draw, f, 6, row_h() - 5, "austere settings", 15, fg,
        bg);

    unsigned col_x[2] = { 6, w / 2 + 6 };
    unsigned col_w[2] = { w / 2 - 12, w / 2 - 12 };
    unsigned y[2] = { row_h() + 2, row_h() + 2 };
    unsigned base = nvis && (unsigned)sel >= nvis
        ? (unsigned)sel - nvis + 1
        : 0;
    unsigned end = base + nvis > nrows ? nrows : base + nvis;

    for (unsigned i = base; i < end; i++) {
        row_t *r = &rows[i];
        int cx = (int)col_x[r->col];
        unsigned cwid = col_w[r->col];
        int by = (int)y[r->col];

        if (r->type == R_HDR) {
            draw_text(wm, &draw, f, cx, by + row_h() - 5, r->label,
                (unsigned)strlen(r->label), BAR_DIM, bg);
            y[r->col] += row_h();
            continue;
        }
        bool is_sel = (int)i == sel;

        if (is_sel)
            draw_rect(wm, &draw, cx - 3, by, cwid, row_h(), ac);
        uint32_t rowbg = is_sel ? ac : bg;
        uint32_t rowfg = is_sel ? bg : fg;

        draw_text(wm, &draw, f, cx, by + row_h() - 5, r->label,
            (unsigned)strlen(r->label), rowfg, rowbg);
        char val[128];
        row_value(r, val, sizeof(val));
        unsigned vw = draw_text_w(wm, f, val, (unsigned)strlen(val));
        draw_text(wm, &draw, f, (int)(cx + cwid - vw), by + row_h() - 5,
            val, (unsigned)strlen(val),
            r->type == R_BIND ? BAR_DIM : rowfg, rowbg);
        y[r->col] += row_h();
    }
    if (nrows > nvis && nvis) {
        unsigned rows_h = h - 2 * row_h();
        unsigned rh = rows_h * nvis / nrows;

        if (rh < 2)
            rh = 2;
        if (rh > rows_h)
            rh = rows_h;
        unsigned rt = (rows_h - rh) * base / (nrows - nvis);

        draw_rect(wm, &draw, (int)(w - 4), (int)(row_h() + rt), 2,
            rh, BAR_DIM);
    }

    /* footer / prompt */
    const char *hint = edit_mode == 2
        ? "press new combo  (Esc cancels)"
        : edit_mode == 1
        ? "value: _  (Return accepts, Esc cancels)"
        : "arrows adjust - Tab column - PgDn scroll - Ctrl+s save - Esc close";
    unsigned fh = h - row_h() + 4;

    draw_rect(wm, &draw, 0, (int)(h - row_h()), w, row_h(), bg);
    draw_text(wm, &draw, f, 6, (int)(fh + 2), hint,
        (unsigned)strlen(hint), edit_mode ? ac : BAR_DIM, bg);
    if (edit_mode == 1)
        draw_text(wm, &draw, f, 60, (int)(fh + 2), editbuf,
            (unsigned)strlen(editbuf), fg, bg);
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

static char **apps_rows;
static unsigned napps_rows;

static void apps_l2_close(wm_t *wm);

static void
apps_rows_free(void)
{
    for (unsigned i = 0; i < napps_rows; i++)
        free(apps_rows[i]);
    free(apps_rows);
    apps_rows = NULL;
    napps_rows = 0;
}

static void
apps_open_cat(wm_t *wm, unsigned cat)
{
    apps_rows_free();
    for (unsigned i = 0; i < apps_count(); i++)
        if (app_category(i) == (int)cat)
            napps_rows++;
    if (!napps_rows)
        return;
    apps_rows = xmalloc(napps_rows * sizeof(char *));
    unsigned k = 0;

    for (unsigned i = 0; i < apps_count(); i++)
        if (app_category(i) == (int)cat)
            apps_rows[k++] = xstrdup(app_name(i));
    sort_strs(apps_rows, napps_rows, true);
    panel_def_t def = {
        .title = app_category_name(cat), .prompt = "", .rows = apps_rows,
        .nrows = napps_rows, .filter = true, .on_enter = apps_l2_enter,
        .on_close = apps_l2_close, .px_w = 320, .anchor_bar = true
    };

    panel_open(wm, &def);
}

static void
apps_l2_close(wm_t *wm)
{
    (void)wm;

    apps_rows_free();
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
        if (nprefix > 1)
            qsort(pview, nprefix, sizeof(char *), cmp_str);
        if (pview_n - nprefix > 1)
            qsort(pview + nprefix, pview_n - nprefix, sizeof(char *),
                cmp_str);
    }
    if (psel >= pview_n)
        psel = pview_n ? pview_n - 1 : 0;
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
        h = (pview_n < (a.h - 3 * row_h()) / row_h()
                ? pview_n : (a.h - 3 * row_h()) / row_h())
            * row_h() + 2 * row_h();
        x = a.x + (int)((a.w - w) / 2);
        y = a.y + (int)((a.h - h) / 2);
    }
    unsigned max_vis = (h - 2 * row_h()) / row_h();
    unsigned nvis = pview_n < max_vis ? pview_n : max_vis;

    if (!pdef.anchor_bar)
        h = nvis * row_h() + 2 * row_h();
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
    uint32_t bg = cfg.bar_bg;
    uint32_t fg = cfg.bar_fg;
    uint32_t ac = cfg.focus_color;

    draw_rect(wm, &draw, 0, 0, w, h, bg);
    draw_rect(wm, &draw, 0, 0, w, row_h(), bg);
    draw_text(wm, &draw, f, 6, row_h() - 5, ptitle,
        (unsigned)strlen(ptitle), fg, bg);

    for (unsigned i = 0; i < nvis; i++) {
        const char *row = pview[base + i];
        bool selected = base + i == psel;

        if (selected)
            draw_rect(wm, &draw, 3, (int)(row_h() + i * row_h()),
                w - 6, row_h(), ac);
        draw_text(wm, &draw, f, 6, (int)(row_h() + i * row_h() + row_h() - 5),
            row, (unsigned)strlen(row),
            selected ? bg : fg,
            selected ? ac : bg);
    }

    /* prompt line with input + optional module hint */
    char line[256];

    int pl = snprintf(line, sizeof(line), "%s", pprompt);

    if (pl < 0)
        pl = 0;
    snprintf(line + pl, sizeof(line) - (size_t)pl, "%s", pinput);
    draw_rect(wm, &draw, 0, (int)(h - row_h()), w, row_h(), bg);
    draw_text(wm, &draw, f, 6, (int)(h - row_h() + row_h() - 5), line,
        (unsigned)strlen(line), fg, bg);
}

static void
panel_gc_take(wm_t *wm, xcb_window_t w)
{
    /* win/gc are shared between the settings window and panel windows;
     * only one is live at a time, but the static draw_t must not
     * accumulate GCs across open/close cycles. */
    if (draw.gc)
        xcb_free_gc(wm->conn, draw.gc);
    draw_setup(wm, &draw, w);
}

static void
panel_gc_drop(wm_t *wm)
{
    if (draw.gc) {
        xcb_free_gc(wm->conn, draw.gc);
        draw.gc = 0;
    }
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
        (uint32_t[]){ cfg.bar_bg, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
                XCB_EVENT_MASK_BUTTON_PRESS });
    draw_setup(wm, &draw, win);
    cur_w = w;
    cur_h = h;
    xcb_map_window(wm->conn, win);
    raise_window(wm, win);
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
    panel_gc_drop(wm);
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

static void
panel_preview(wm_t *wm)
{
    if (!pdef.on_preview || !pview_n)
        return;
    pdef.on_preview(wm, pview[psel]);
    /* live switch raised the target client; keep the panel on top */
    raise_window(wm, win);
}

static void
panel_key(wm_t *wm, xcb_key_press_event_t *ev)
{
    xcb_keysym_t sym = xcb_key_symbols_get_keysym(wm->keysyms,
        ev->detail, 0);
    bool moved = false;

    switch (sym) {
    case 0xff1b: /* Escape */
        panel_finish(wm, false);
        return;
    case 0xff0d: /* Return */
        panel_finish(wm, true);
        return;
    case 0xff52: /* Up */
        if (pview_n) {
            psel = psel == 0 ? pview_n - 1 : psel - 1;
            moved = true;
        }
        break;
    case 0xff54: /* Down */
        if (pview_n) {
            psel = psel + 1 == pview_n ? 0 : psel + 1;
            moved = true;
        }
        break;
    case 0xff09: /* Tab */
        /* held-alt Tab cycles the selection (the switcher gesture);
         * plain Tab keeps the launcher completion behaviour. */
        if (ev->state & XCB_MOD_MASK_1) {
            if (pview_n) {
                psel = ev->state & XCB_MOD_MASK_SHIFT
                    ? (psel == 0 ? pview_n - 1 : psel - 1)
                    : (psel + 1 == pview_n ? 0 : psel + 1);
                moved = true;
            }
            break;
        }
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
            moved = true;
        }
        break;
    default:
        if (panel_text_key(wm, sym))
            return;
        return;
    }
    if (moved)
        panel_preview(wm);
    menu_draw(wm);
}

/* ---- window lifecycle ---------------------------------------------- */

static void
menu_create_window(wm_t *wm)
{
    monitor_t *mon = focused_mon(wm);
    unsigned w = mon->geom.w / 2;
    unsigned h, nvis;

    menu_metrics(wm, &h, &nvis);
    Rect a = mon_workarea(mon);

    win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, win,
        wm->scr->root,
        (int16_t)(a.x + (a.w - w) / 2), (int16_t)(a.y + (a.h - h) / 2),
        (uint16_t)w, (uint16_t)h, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
            XCB_CW_EVENT_MASK,
        (uint32_t[]){ cfg.bar_bg, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
            XCB_EVENT_MASK_KEY_RELEASE });
    panel_gc_take(wm, win);
    cur_w = w;
    cur_h = h;
    xcb_map_window(wm->conn, win);
    raise_window(wm, win);
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
    if (psel < def->init_sel && def->init_sel < pview_n)
        psel = def->init_sel;
    panel_create_window(wm);
    xcb_grab_keyboard(wm->conn, 0, win, XCB_CURRENT_TIME,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    is_open = true;
    if (def->init_sel && pview_n > 1)
        panel_preview(wm);
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
    panel_gc_drop(wm);
    xcb_destroy_window(wm->conn, win);
    win = XCB_NONE;
    is_open = false;
    panel_mode = false;
    arrange(wm);
    bar_render_all(wm);
}

/* The panel/overlay window died beneath us (e.g. substructure-driven
 * DestroyNotify): release the grab so input is never left hostage and
 * clear state so menu_active() cannot wedge the key path on a dead
 * window. */
void
menu_window_gone(wm_t *wm, xcb_window_t w)
{
    if (overlay_active && w == overlay.win) {
        xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
        overlay_active = false;
        if (overlay.close)
            overlay.close(wm);
        overlay.win = XCB_NONE;
        arrange(wm);
        bar_render_all(wm);
    } else if (is_open && w == win) {
        xcb_ungrab_keyboard(wm->conn, XCB_CURRENT_TIME);
        panel_gc_drop(wm);
        xcb_destroy_window(wm->conn, win); /* no-op if already gone */
        win = XCB_NONE;
        is_open = false;
        panel_mode = false;
        arrange(wm);
        bar_render_all(wm);
    }
}

bool
menu_active(void)
{
    return is_open || overlay_active;
}

/* A focus change or floater re-raise while a panel is open must not
 * bury it (launcher/settings have no preview to re-raise behind). */
void
menu_bump(wm_t *wm)
{
    if (!is_open && !overlay_active)
        return;
    raise_window(wm, overlay_active ? overlay.win : win);
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
    settings_reapply_clients(wm);
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

        if (end != editbuf) {
            if (v < r->dlo)
                v = r->dlo;
            if (v > r->dhi)
                v = r->dhi;
            *r->dval = v;
        }
        break;
    }
    case R_COLOR: {
        uint32_t c;

        if (conf_parse_color(editbuf, &c))
            *r->uval = c;
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
        double v = *r->dval + dir * r->dstep;

        if (v >= r->dlo && v <= r->dhi)
            *r->dval = v;
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
nav_page(wm_t *wm, int dir)
{
    unsigned nvis, h;

    menu_metrics(wm, &h, &nvis);
    if (!nvis)
        return;
    for (unsigned i = 0; i < nvis; i++)
        nav(dir);
}

static void
nav_edge(int dir)
{
    int want = rows[sel].col;

    if (dir < 0) {
        for (int i = 0; i < (int)nrows; i++)
            if (rows[i].col == want && rows[i].type != R_HDR &&
                i < sel) {
                sel = i;
                return;
            }
        for (int i = (int)nrows - 1; i >= 0; i--)
            if (rows[i].col == want && rows[i].type != R_HDR)
                sel = i;
    } else {
        for (int i = (int)nrows - 1; i >= 0; i--)
            if (rows[i].col == want && rows[i].type != R_HDR &&
                i > sel)
                sel = i;
        for (int i = 0; i < (int)nrows; i++)
            if (rows[i].col == want && rows[i].type != R_HDR)
                sel = i;
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
        /* Grab bare keys system-wide and swallow every keystroke; a
         * real modifier is required to keep the WM usable. */
        unsigned mod_only = (unsigned)(state &
            (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL |
            XCB_MOD_MASK_1 | XCB_MOD_MASK_4));

        if (!mod_only) {
            popup_notify(wm, "hold a modifier while pressing a key");
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
    case 0xff55: /* PageUp */
        nav_page(wm, -1);
        break;
    case 0xff56: /* PageDown */
        nav_page(wm, 1);
        break;
    case 0xff50: /* Home */
        nav_edge(-1);
        break;
    case 0xff57: /* End */
        nav_edge(1);
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
menu_key_release(wm_t *wm, xcb_key_release_event_t *ev)
{
    xcb_keysym_t sym;

    if (!is_open)
        return;
    sym = xcb_key_symbols_get_keysym(wm->keysyms, ev->detail, 0);
    /* hold-alt gesture for open panels: closing on the opener's Alt
     * release. Other key releases leave the panel alone. */
    if (sym != 0xffe9 && sym != 0xffea) /* Alt_L / Alt_R */
        return;
    if (overlay_active) {
        menu_pop_overlay(wm);
        return;
    }
    if (panel_mode && pdef.hold_alt)
        panel_finish(wm, false);
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

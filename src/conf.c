#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "actions.h"
#include "barmod.h"
#include "conf.h"
#include "draw.h"
#include "event.h"
#include "keys.h"
#include "popup.h"
#include "util.h"

#include <tomlc17.h>

/* §9: the config file is one view over the Settings struct. Parsing
 * validates every value; per-entry failures warn and keep the default
 * at startup, while a reload (strict) aborts wholesale on any error. */

static char path_buf[512];
static bool path_resolved;

const char *
conf_path(void)
{
    if (path_resolved)
        return path_buf;
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg && *xdg)
        snprintf(path_buf, sizeof(path_buf),
            "%s/austere/austere.conf", xdg);
    else
        snprintf(path_buf, sizeof(path_buf),
            "%s/.config/austere/austere.conf",
            getenv("HOME") ? getenv("HOME") : "/root");
    path_resolved = true;
    return path_buf;
}

static void
warn_at(const toml_datum_t *d, const char *what)
{
    if (d && d->lineno > 0)
        fprintf(stderr, "austere: conf:%d:%d: %s\n", d->lineno,
            d->colno, what);
    else
        fprintf(stderr, "austere: conf: %s\n", what);
}

/* Validate an option datum: absent keys are fine (caller skips),
 * invalid values warn and flag the load in strict mode. Callers pass a
 * validity predicate whose value/union reads are type-guarded. */
static bool
get_ok(toml_datum_t *d, bool valid, const char *key, bool *bad, bool strict)
{
    if (d->type == TOML_UNKNOWN)
        return false;
    if (!valid) {
        warn_at(d, key);
        if (strict)
            *bad = true;
        return false;
    }
    return true;
}

static bool
get_str(toml_datum_t tab, const char *key, char **out,
    bool *bad, bool strict)
{
    toml_datum_t d = toml_get(tab, key);

    if (!get_ok(&d, d.type == TOML_STRING, key, bad, strict))
        return true;
    free(*out);
    *out = xstrdup(d.u.s);
    return true;
}

static bool
get_bool(toml_datum_t tab, const char *key, bool *out,
    bool *bad, bool strict)
{
    toml_datum_t d = toml_get(tab, key);

    if (!get_ok(&d, d.type == TOML_BOOLEAN, key, bad, strict))
        return true;
    *out = d.u.boolean;
    return true;
}

static bool
get_int(toml_datum_t tab, const char *key, long lo, long hi,
    unsigned *out, bool *bad, bool strict)
{
    toml_datum_t d = toml_get(tab, key);

    if (!get_ok(&d, d.type == TOML_INT64 && d.u.int64 >= lo
        && d.u.int64 <= hi, key, bad, strict))
        return true;
    *out = (unsigned)d.u.int64;
    return true;
}

static bool
get_dbl(toml_datum_t tab, const char *key, double lo,
    double hi, double *out, bool *bad, bool strict)
{
    toml_datum_t d = toml_get(tab, key);
    double v = d.type == TOML_FP64 ? d.u.fp64
        : (d.type == TOML_INT64 ? (double)d.u.int64 : -1);

    if (!get_ok(&d, d.type != TOML_UNKNOWN && v >= lo && v <= hi,
        key, bad, strict))
        return true;
    *out = v;
    return true;
}

bool
conf_parse_color(const char *s, uint32_t *out)
{
    if (!s)
        return false;
    if (*s == '#')
        s++;
    size_t len = strlen(s);
    char *end;
    long v = strtol(s, &end, 16);

    return (len == 6 || len == 8) && !*end && v >= 0 &&
        v <= 0xffffff
        ? (*out = (uint32_t)v, true)
        : false;
}

static bool
get_color(toml_datum_t tab, const char *key, uint32_t *out,
    bool *bad, bool strict)
{
    toml_datum_t d = toml_get(tab, key);

    get_ok(&d, d.type == TOML_STRING && conf_parse_color(d.u.s, out),
        key, bad, strict);
    return true;
}

static bool
get_enum(toml_datum_t tab, const char *key,
    const char *const *vals, unsigned *out, bool *bad, bool strict)
{
    toml_datum_t d = toml_get(tab, key);
    unsigned i = 0;

    while (d.type == TOML_STRING && vals[i] &&
        strcmp(vals[i], d.u.s))
        i++;
    if (!get_ok(&d, d.type == TOML_STRING && vals[i],
        key, bad, strict))
        return true;
    *out = i;
    return true;
}

/* Grow *list by one and store a strdup of s at the new tail. */
static bool
strlist_add(char ***list, unsigned *n, const char *s)
{
    char **nl = realloc(*list, (*n + 1) * sizeof(char *));

    if (!nl)
        return false;
    *list = nl;
    (*list)[(*n)++] = xstrdup(s);
    return true;
}

/* [bar] modules_left/center/right = ["name", ...]: ordered module
 * groups. Unknown names abort a strict load so a typo can't silently
 * drop a monitor's modules. */
static void
get_bar_mods(toml_datum_t tab, const char *key, char ***out, unsigned *n,
    bool *bad, bool strict)
{
    toml_datum_t arr = toml_get(tab, key);

    if (arr.type == TOML_UNKNOWN)
        return;
    if (arr.type != TOML_ARRAY) {
        warn_at(&arr, "[bar] must be an array");
        if (strict)
            *bad = true;
        return;
    }
    for (int32_t i = 0; i < arr.u.arr.size; i++) {
        if (arr.u.arr.elem[i].type != TOML_STRING)
            continue;
        const char *name = arr.u.arr.elem[i].u.s;

        if (!mod_lookup(name)) {
            fprintf(stderr, "austere: conf: [bar] %s: unknown module "
                "'%s'\n", key, name);
            if (strict)
                *bad = true;
            return;
        }
        strlist_add(out, n, name);
    }
}

/* [keys] bind = ["combo action", ...] */
static void
get_binds(toml_datum_t tab, settings_t *s, bool *bad, bool strict)
{
    toml_datum_t arr = toml_get(tab, "bind");

    if (arr.type == TOML_UNKNOWN)
        return;
    if (arr.type != TOML_ARRAY) {
        warn_at(&arr, "[keys] bind must be an array");
        if (strict)
            *bad = true;
        return;
    }
    s->nbinds = 0;
    for (int32_t i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t e = arr.u.arr.elem[i];

        if (e.type != TOML_STRING) {
            warn_at(&e, "bind entry must be a string");
            if (strict)
                *bad = true;
            continue;
        }
        char buf[BIND_CMD_MAX + 128];

        snprintf(buf, sizeof(buf), "%s", e.u.s);
        char *sp = strchr(buf, ' ');

        if (!sp) {
            warn_at(&e, "bind needs 'combo action'");
            if (strict)
                *bad = true;
            continue;
        }
        *sp = '\0';
        const char *combo = buf;
        const char *action = sp + 1;
        char *sp2 = strchr(action, ' ');
        size_t alen = sp2 ? (size_t)(sp2 - action) : strlen(action);
        char act[16];

        /* exec / set_layout / load_state binds keep everything past
         * the action word as their argument; other actions take a
         * short scalar arg */
        bool is_cmd_act =
            (alen == 4 && !strncmp(action, "exec", 4)) ||
            (alen == 10 && !strncmp(action, "set_layout", 10)) ||
            (alen == 10 && !strncmp(action, "load_state", 10));
        char argbuf[32] = "";
        char cmdrest[BIND_CMD_MAX] = "";

        if (is_cmd_act) {
            if (sp2) {
                char *tail = sp2 + 1;

                while (*tail == ' ')
                    tail++;
                snprintf(cmdrest, sizeof(cmdrest), "%s", tail);
            }
            snprintf(act, sizeof(act), "%.*s", (int)alen, action);
            action = act;
        } else if (sp2) {
            snprintf(argbuf, sizeof(argbuf), "%s", sp2 + 1);
            *(char *)sp2 = '\0';
        }
        uint8_t id = action_lookup(action);

        if (id == 255) {
            warn_at(&e, "unknown action");
            if (strict)
                *bad = true;
            continue;
        }
        bind_t b;

        if (!parse_bind(combo, action, &b)) {
            warn_at(&e, "unknown keysym or modifier");
            if (strict)
                *bad = true;
            continue;
        }
        if (id == ACT_VIEW_WS || id == ACT_SEND_WS) {
            char *end;
            long v = strtol(argbuf, &end, 10);

            if (*argbuf < '0' || *end || v < 0 || v >= WS_MAX) {
                warn_at(&e, "ws action needs index 0..8");
                if (strict)
                    *bad = true;
                continue;
            }
            b.arg = (int)v;
        } else if (id == ACT_EXEC) {
            if (!*cmdrest) {
                warn_at(&e, "exec bind needs a command");
                if (strict)
                    *bad = true;
                continue;
            }
            snprintf(b.cmd, sizeof(b.cmd), "%s", cmdrest);
        } else if (id == ACT_SET_LAYOUT || id == ACT_LOAD_STATE) {
            if (!*cmdrest) {
                warn_at(&e, "set_layout/load_state bind needs a name");
                if (strict)
                    *bad = true;
                continue;
            }
            snprintf(b.cmd, sizeof(b.cmd), "%s", cmdrest);
        }
        if (s->nbinds < MAX_BINDS)
            s->binds[s->nbinds++] = b;
    }
}

static void
get_ws_names(toml_datum_t tab, settings_t *s, bool *bad, bool strict)
{
    toml_datum_t arr = toml_get(tab, "names");

    if (arr.type == TOML_UNKNOWN)
        return;
    if (arr.type != TOML_ARRAY) {
        warn_at(&arr, "[workspaces] names must be an array");
        if (strict)
            *bad = true;
        return;
    }
    for (int32_t i = 0; i < arr.u.arr.size; i++) {
        toml_datum_t e = arr.u.arr.elem[i];

        if (e.type != TOML_STRING) {
            warn_at(&e, "workspace name must be a string");
            if (strict)
                *bad = true;
            continue;
        }
        if (i >= WS_MAX) {
            warn_at(&e, "extra workspace name ignored");
            break;
        }
        free(s->ws_names[i]);
        s->ws_names[i] = xstrdup(e.u.s);
    }
}

bool
conf_load(const char *path, settings_t *s, bool strict)
{
    if (access(path, R_OK) != 0) {
        /* tomlc17 reports a missing file as an empty-but-ok document,
         * which would silently discard every setting */
        fprintf(stderr, "austere: conf: %s: %s\n", path,
            strerror(errno));
        return false;
    }
    toml_result_t r = toml_parse_file_ex(path);
    bool bad = false;

    if (!r.ok) {
        fprintf(stderr, "austere: conf: %s\n", r.errmsg);
        toml_free(r);
        return false;
    }

    toml_datum_t gen = toml_get(r.toptab, "general");
    toml_datum_t app = toml_get(r.toptab, "appearance");
    toml_datum_t dec = toml_get(r.toptab, "deco");
    toml_datum_t bar = toml_get(r.toptab, "bar");
    toml_datum_t beh = toml_get(r.toptab, "behavior");
    toml_datum_t lay = toml_get(r.toptab, "layouts");
    toml_datum_t ws = toml_get(r.toptab, "workspaces");
    toml_datum_t keys = toml_get(r.toptab, "keys");
    toml_datum_t mouse = toml_get(r.toptab, "mouse");
    toml_datum_t wp = toml_get(r.toptab, "wallpaper");
    toml_datum_t sw = toml_get(r.toptab, "switcher");
    toml_datum_t at = toml_get(r.toptab, "autostart");
    toml_datum_t lnch = toml_get(r.toptab, "launcher");

    if (gen.type == TOML_TABLE) {
        get_str(gen, "terminal", &s->terminal, &bad, strict);
        get_bool(gen, "socket", &s->socket, &bad, strict);
    }
    if (app.type == TOML_TABLE) {
        get_int(app, "border_width", 0, 32, &s->border_width, &bad,
            strict);
        get_color(app, "focus_color", &s->focus_color, &bad, strict);
        get_color(app, "unfocus_color", &s->unfocus_color, &bad,
            strict);
        get_color(app, "urgent_color", &s->urgent_color, &bad, strict);
        get_int(app, "gap", 0, 128, &s->gap, &bad, strict);
        get_bool(app, "smart_gaps", &s->smart_gaps, &bad, strict);
        get_int(app, "corner_radius", 0, 64, &s->corner_radius, &bad,
            strict);
        get_str(app, "font", &s->font, &bad, strict);
    }
    if (dec.type == TOML_TABLE) {
        get_bool(dec, "deco", &s->deco, &bad, strict);
        get_int(dec, "deco_title_h", 10, 40, &s->deco_title_h, &bad,
            strict);
        get_color(dec, "deco_border", &s->deco_border, &bad, strict);
        get_color(dec, "deco_unfocus_border", &s->deco_unfocus_border,
            &bad, strict);
    }
    if (bar.type == TOML_TABLE) {
        static const char *const pos[] = { "top", "bottom", NULL };

        unsigned pi = s->bar_bottom ? 1 : 0;

        get_enum(bar, "position", pos, &pi, &bad, strict);
        s->bar_bottom = pi == 1;
        get_str(bar, "time_format", &s->time_format, &bad, strict);
        get_color(bar, "bar_bg", &s->bar_bg, &bad, strict);
        get_color(bar, "bar_fg", &s->bar_fg, &bad, strict);
        get_int(bar, "bar_gap", 0, 128, &s->bar_gap, &bad, strict);
        get_bar_mods(bar, "modules_left", &s->bar_left, &s->nbar_left,
            &bad, strict);
        get_bar_mods(bar, "modules_center", &s->bar_center,
            &s->nbar_center, &bad, strict);
        get_bar_mods(bar, "modules_right", &s->bar_right,
            &s->nbar_right, &bad, strict);
    }
    if (beh.type == TOML_TABLE) {
        get_bool(beh, "focus_follows_mouse", &s->focus_follows_mouse,
            &bad, strict);
        get_bool(beh, "raise_on_click", &s->raise_on_click, &bad,
            strict);
        get_int(beh, "snap_distance", 0, 256, &s->snap_distance, &bad,
            strict);
        get_int(beh, "popup_timeout", 1, 60, &s->popup_timeout, &bad,
            strict);
        get_bool(beh, "swallowing", &s->swallowing, &bad, strict);
    }
    if (lay.type == TOML_TABLE) {
        get_str(lay, "default", &s->default_layout, &bad, strict);
        get_int(lay, "nmaster", 1, WS_MAX, &s->nmaster, &bad, strict);
        get_dbl(lay, "split_ratio", 0.1, 0.9, &s->split_ratio, &bad,
            strict);
        get_dbl(lay, "ratio_step", 0.01, 0.5, &s->ratio_step, &bad,
            strict);
    }
    if (ws.type == TOML_TABLE) {
        get_ws_names(ws, s, &bad, strict);    }
    if (keys.type == TOML_TABLE)
        get_binds(keys, s, &bad, strict);
    if (wp.type == TOML_TABLE) {
        get_str(wp, "setter_command", &s->wp_setter, &bad, strict);
        toml_datum_t dirs = toml_get(wp, "dirs");

        if (dirs.type == TOML_ARRAY) {
            for (int32_t i = 0; i < dirs.u.arr.size; i++) {
                if (dirs.u.arr.elem[i].type != TOML_STRING)
                    continue;
                strlist_add(&s->wp_dirs, &s->nwp_dirs,
                    dirs.u.arr.elem[i].u.s);
            }
        }
    }
    if (at.type == TOML_TABLE) {
        toml_datum_t run = toml_get(at, "run");

        if (run.type == TOML_ARRAY) {
            for (int32_t i = 0; i < run.u.arr.size; i++) {
                if (run.u.arr.elem[i].type != TOML_STRING)
                    continue;
                strlist_add(&s->autostart_run, &s->nautostart_run,
                    run.u.arr.elem[i].u.s);
            }
        }
    }
    if (sw.type == TOML_TABLE) {
        static const char *const scopes[] = { "all", "monitor", NULL };

        unsigned si = s->switcher_monitor_scope ? 1 : 0;

        get_enum(sw, "scope", scopes, &si, &bad, strict);
        s->switcher_monitor_scope = si == 1;
        get_bool(sw, "live_preview", &s->switcher_live_preview, &bad,
            strict);
    }
    if (lnch.type == TOML_TABLE) {
        get_bool(lnch, "scan_path", &s->launcher_scan_path, &bad,
            strict);
        get_str(lnch, "custom_dir", &s->launcher_custom_dir, &bad,
            strict);
        get_int(lnch, "history_size", 0, 500, &s->launcher_history_size,
            &bad, strict);
        get_str(lnch, "default_module", &s->launcher_default_module,
            &bad, strict);

        toml_datum_t arr = toml_get(lnch, "entry");

        if (arr.type == TOML_ARRAY) {
            for (int32_t i = 0; i < arr.u.arr.size; i++) {
                if (arr.u.arr.elem[i].type != TOML_STRING)
                    continue;
                strlist_add(&s->launcher_entries, &s->nlauncher_entries,
                    arr.u.arr.elem[i].u.s);
            }
        }
        toml_datum_t mods = toml_get(lnch, "module");

        if (mods.type == TOML_ARRAY) { /* array of tables */
            for (int32_t i = 0; i < mods.u.arr.size; i++) {
                toml_datum_t t = mods.u.arr.elem[i];

                if (t.type != TOML_TABLE)
                    continue;
                toml_datum_t dn = toml_get(t, "name");
                toml_datum_t dd = toml_get(t, "desc");
                toml_datum_t dc = toml_get(t, "cmd");

                if (dn.type != TOML_STRING || dc.type != TOML_STRING)
                    continue;
                /* Grow all three parallel arrays before committing any
                 * entry so a partial realloc never desyncs them. */
                char **names = realloc(s->mod_name,
                    (s->nmodules + 1) * sizeof(char *));
                char **descs = realloc(s->mod_desc,
                    (s->nmodules + 1) * sizeof(char *));
                char **cmds = realloc(s->mod_cmd,
                    (s->nmodules + 1) * sizeof(char *));

                if (!names || !descs || !cmds)
                    break;
                s->mod_name = names;
                s->mod_desc = descs;
                s->mod_cmd = cmds;
                s->mod_name[s->nmodules] = xstrdup(dn.u.s);
                s->mod_desc[s->nmodules] =
                    dd.type == TOML_STRING ? xstrdup(dd.u.s)
                                           : xstrdup("");
                s->mod_cmd[s->nmodules] = xstrdup(dc.u.s);
                s->nmodules++;
            }
        }
    }
    if (mouse.type == TOML_TABLE) {
        static const char *const modnames[] = { "alt", "super", NULL };

        unsigned mi = s->mouse_mods == XCB_MOD_MASK_1 ? 0 : 1;

        get_enum(mouse, "modifier", modnames, &mi, &bad, strict);
        s->mouse_mods = mi == 0 ? XCB_MOD_MASK_1 : XCB_MOD_MASK_4;
        get_int(mouse, "move_button", 1, 5, &s->move_button, &bad,
            strict);
        get_int(mouse, "resize_button", 1, 5, &s->resize_button, &bad,
            strict);
    }

    toml_free(r);
    if (bad && strict)
        return false;
    return true;
}

/* Escape a string for TOML basic-string output: backslash, double-quote,
 * and control characters (U+0000..U+001F) must be escaped. Never writes
 * past outsz-1; over-long inputs are truncated (lossy but valid TOML). */
static void
toml_escape(char *out, size_t outsz, const char *s)
{
    char *d = out;
    size_t left = outsz ? outsz - 1 : 0;

    for (const unsigned char *p = (const unsigned char *)s; *p && left;
        p++) {
        size_t n = 1;
        char esc = 0, h0 = 0, h1 = 0;

        if (*p == '\\' || *p == '"') {
            n = 2;
            esc = (char)*p;
        } else if (*p < 0x20) {
            if (*p == '\n' || *p == '\r' || *p == '\t' ||
                *p == '\b' || *p == '\f') {
                n = 2;
                esc = "nrtbf"[*p == '\n' ? 0 : *p == '\r' ? 1 :
                    *p == '\t' ? 2 : *p == '\b' ? 3 : 4];
            } else {
                n = 6;
                h0 = "0123456789abcdef"[(*p >> 4) & 0xf];
                h1 = "0123456789abcdef"[*p & 0xf];
            }
        }
        if (n > (size_t)left)
            break; /* truncate: drop this char rather than split it */
        if (n == 2) {
            *d++ = '\\';
            *d++ = esc;
        } else if (n == 6) {
            *d++ = '\\';
            *d++ = 'u';
            *d++ = '0';
            *d++ = '0';
            *d++ = h0;
            *d++ = h1;
        } else {
            *d++ = (char)*p;
        }
        left -= n;
    }
    *d = '\0';
}

/* Canonical serializer (§9.2): the file becomes machine-canonical
 * after the first save; hand comments are not preserved. */
bool
conf_write(const char *path, const settings_t *s)
{
    char tmp[560];

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");

    if (!f)
        return false;

    /* Escape all user-provided strings up front.  512 bytes is ample
     * for any setting value; longer strings are truncated (safe — the
     * resulting TOML is just lossy, not invalid). */
    char esc_term[512], esc_font[512], esc_time[512], esc_layout[512];
    char esc_custom[512], esc_dmod[512], esc_wp[512];

    toml_escape(esc_term, sizeof(esc_term),
        s->terminal ? s->terminal : "xterm");
    toml_escape(esc_font, sizeof(esc_font),
        s->font ? s->font : DRAW_DEFAULT_FONT);
    toml_escape(esc_time, sizeof(esc_time),
        s->time_format ? s->time_format : "%H:%M");
    toml_escape(esc_layout, sizeof(esc_layout),
        s->default_layout ? s->default_layout : "tile");
    toml_escape(esc_custom, sizeof(esc_custom),
        s->launcher_custom_dir ? s->launcher_custom_dir : "");
    toml_escape(esc_dmod, sizeof(esc_dmod),
        s->launcher_default_module ? s->launcher_default_module : "");
    toml_escape(esc_wp, sizeof(esc_wp), s->wp_setter ? s->wp_setter : "");

    char hex[8][8];

#define HEXCOL(i, v) (snprintf(hex[i], 8, "#%06x", (unsigned)(s->v)), hex[i])
    fprintf(f,
        "# austere configuration\n"
        "# Edited by hand or via the settings menu; apply with reload.\n"
        "\n[general]\nterminal = \"%s\"\nsocket = %s\n\n"
        "[appearance]\nborder_width = %u\ncorner_radius = %u\n"
        "focus_color = \"%s\"\nunfocus_color = \"%s\"\n"
        "urgent_color = \"%s\"\ngap = %u\n"
        "smart_gaps = %s\nfont = \"%s\"\n\n"
        "[deco]\ndeco = %s\ndeco_title_h = %u\ndeco_border = \"%s\"\n"
        "deco_unfocus_border = \"%s\"\n\n"
        "[bar]\nposition = \"%s\"\ntime_format = \"%s\"\n"
        "bar_bg = \"%s\"\nbar_fg = \"%s\"\nbar_gap = %u",
        esc_term, s->socket ? "true" : "false",
        s->border_width, s->corner_radius,
        HEXCOL(0, focus_color), HEXCOL(1, unfocus_color),
        HEXCOL(2, urgent_color), s->gap, s->smart_gaps ? "true" : "false",
        esc_font, s->deco ? "true" : "false",
        s->deco_title_h,
        HEXCOL(5, deco_border), HEXCOL(6, deco_unfocus_border),
        s->bar_bottom ? "bottom" : "top", esc_time,
        HEXCOL(3, bar_bg), HEXCOL(4, bar_fg), s->bar_gap);
    fprintf(f, "\nmodules_left = [");
    for (unsigned i = 0; i < s->nbar_left; i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", s->bar_left[i]);
    fprintf(f, "]\nmodules_center = [");
    for (unsigned i = 0; i < s->nbar_center; i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", s->bar_center[i]);
    fprintf(f, "]\nmodules_right = [");
    for (unsigned i = 0; i < s->nbar_right; i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", s->bar_right[i]);
    fprintf(f, "]\n\n");
    fprintf(f,
        "[behavior]\nfocus_follows_mouse = %s\nraise_on_click = %s\n"
        "snap_distance = %u\npopup_timeout = %u\n"
        "swallowing = %s\n\n"
        "[layouts]\ndefault = \"%s\"\nnmaster = %u\n"
        "split_ratio = %.2f\nratio_step = %.2f\n\n",
        s->focus_follows_mouse ? "true" : "false",
        s->raise_on_click ? "true" : "false",
        s->snap_distance, s->popup_timeout,
        s->swallowing ? "true" : "false",
        esc_layout, s->nmaster, s->split_ratio, s->ratio_step);
#undef HEXCOL

    fprintf(f, "[workspaces]\nnames = [");
    for (unsigned i = 0; i < WS_MAX; i++) {
        char esc_ws[256];

        toml_escape(esc_ws, sizeof(esc_ws),
            s->ws_names[i] ? s->ws_names[i] : "");
        fprintf(f, "%s\"%s\"", i ? ", " : "", esc_ws);
    }
    fprintf(f, "]\n\n[keys]\nbind = [\n");
    for (unsigned i = 0; i < s->nbinds; i++) {
        char combo[64] = "";
        const bind_t *b = &s->binds[i];

        if (b->mods & XCB_MOD_MASK_4)
            strcat(combo, "super+");
        if (b->mods & XCB_MOD_MASK_CONTROL)
            strcat(combo, "ctrl+");
        if (b->mods & XCB_MOD_MASK_1)
            strcat(combo, "alt+");
        if (b->mods & XCB_MOD_MASK_SHIFT)
            strcat(combo, "shift+");
        keysym_name(b->keysym, combo + strlen(combo),
            sizeof(combo) - strlen(combo));
        if (b->action == ACT_VIEW_WS || b->action == ACT_SEND_WS)
            fprintf(f, "    \"%s %s %d\"%s\n", combo,
                action_name(b->action), b->arg,
                i + 1 < s->nbinds ? "," : "");
        else if (b->action == ACT_EXEC || b->action == ACT_SET_LAYOUT ||
            b->action == ACT_LOAD_STATE) {
            char esc_cmd[256];

            /* cmd is post-TOML-unescape shell text (quotes, backslashes,
             * newlines); save it escaped or the next load aborts. */
            toml_escape(esc_cmd, sizeof(esc_cmd), b->cmd);
            fprintf(f, "    \"%s %s %s\"%s\n", combo,
                action_name(b->action), esc_cmd,
                i + 1 < s->nbinds ? "," : "");
        } else
            fprintf(f, "    \"%s %s\"%s\n", combo,
                action_name(b->action),
                i + 1 < s->nbinds ? "," : "");
    }
    fprintf(f, "]\n\n[switcher]\nscope = \"%s\"\n"
        "live_preview = %s\n\n",
        s->switcher_monitor_scope ? "monitor" : "all",
        s->switcher_live_preview ? "true" : "false");

    fprintf(f,
        "[launcher]\nscan_path = %s\nhistory_size = %u\n",
        s->launcher_scan_path ? "true" : "false",
        s->launcher_history_size);
    if (s->launcher_custom_dir && esc_custom[0])
        fprintf(f, "custom_dir = \"%s\"\n", esc_custom);
    if (s->launcher_default_module && esc_dmod[0])
        fprintf(f, "default_module = \"%s\"\n", esc_dmod);
    if (s->nlauncher_entries) {
        fprintf(f, "entry = [");
        for (unsigned i = 0; i < s->nlauncher_entries; i++) {
            char esc_ent[512];

            toml_escape(esc_ent, sizeof(esc_ent),
                s->launcher_entries[i]);
            fprintf(f, "%s\"%s\"", i ? ", " : "", esc_ent);
        }
        fprintf(f, "]\n");
    }
    fprintf(f, "\n");
    for (unsigned i = 0; i < s->nmodules; i++) {
        char esc_mn[256], esc_md[256], esc_mc[512];

        toml_escape(esc_mn, sizeof(esc_mn), s->mod_name[i]);
        toml_escape(esc_md, sizeof(esc_md), s->mod_desc[i]);
        toml_escape(esc_mc, sizeof(esc_mc), s->mod_cmd[i]);
        fprintf(f,
            "[[launcher.module]]\nname = \"%s\"\ndesc = \"%s\"\n"
            "cmd = \"%s\"\n\n",
            esc_mn, esc_md, esc_mc);
    }

    fprintf(f, "[wallpaper]\nsetter_command = \"%s\"\n", esc_wp);
    if (s->nwp_dirs) {
        fprintf(f, "dirs = [");
        for (unsigned i = 0; i < s->nwp_dirs; i++) {
            char esc_d[512];

            toml_escape(esc_d, sizeof(esc_d), s->wp_dirs[i]);
            fprintf(f, "%s\"%s\"", i ? ", " : "", esc_d);
        }
        fprintf(f, "]\n");
    }
    if (s->nautostart_run) {
        fprintf(f, "\n[autostart]\nrun = [");
        for (unsigned i = 0; i < s->nautostart_run; i++) {
            char esc_a[512];

            toml_escape(esc_a, sizeof(esc_a), s->autostart_run[i]);
            fprintf(f, "%s\"%s\"", i ? ", " : "", esc_a);
        }
        fprintf(f, "]\n");
    }
    fprintf(f, "\n[mouse]\nmodifier = \"%s\"\nmove_button = %u\n"
        "resize_button = %u\n",
        s->mouse_mods == XCB_MOD_MASK_1 ? "alt" : "super",
        s->move_button, s->resize_button);

    fclose(f);
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/* §9.5: first run writes the commented canonical default so the user's
 * first edit surface exists before their first keystroke. */
bool
conf_ensure(const char *path)
{
    if (access(path, F_OK) == 0)
        return true;

    char dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash)
        *slash = '\0';
    if (mkdir_p(dir) < 0)
        return false;

    FILE *f = fopen(path, "w");

    if (!f)
        return false;
fprintf(f,
        "# Austere configuration — created on first run.\n"
        "# All keys are optional; delete any and the default applies.\n"
        "# There is no file-watch: to apply a file, press super+Escape\n"
        "# (reload), send SIGHUP, or pick a config state with super+grave.\n"
        "\n[general]\nterminal = \"kitty\"\nsocket = true\n"
        "\n[appearance]\nborder_width = 2\n"
        "focus_color = \"#5f819d\"\nunfocus_color = \"#444444\"\n"
        "urgent_color = \"#af3f3f\"\ngap = 0\n"
        "smart_gaps = false           # gaps vanish when one visible client\n"
        "corner_radius = 0            # >0 rounds clients and decorations\n"
        "font = \"Agave Nerd Font Mono:pixelsize=16\"  # any fontconfig pattern\n"
        "\n[deco]\ndeco = false                 # title bars on all windows\n"
        "deco_title_h = 20            # 10..40\n"
        "deco_border = \"#5f819d\"       # focused decoration border\n"
        "deco_unfocus_border = \"#444444\"\n"
        "\n[bar]\nposition = \"top\"          # top | bottom\n"
        "time_format = \"%%a %%d %%b %%H:%%M\"\nbar_bg = \"#1a1a1a\"\n"
        "bar_fg = \"#cccccc\"\nbar_gap = 0\n"
        "modules_left = [\"workspaces\", \"layout\"]\n"
        "modules_center = [\"title\"]\n"
        "modules_right = [\"cpu\", \"ram\", \"battery\", \"volume\", "
        "\"clock\"]\n"
        "\n[behavior]\nfocus_follows_mouse = true\n"
        "raise_on_click = true\nsnap_distance = 12\npopup_timeout = 5\n"
        "swallowing = false           # adopt windows spawned by terminal\n"
        "\n[layouts]\ndefault = \"tile\"        # tile | monocle\n"
        "nmaster = 1\nsplit_ratio = 0.50\nratio_step = 0.05\n"
        "\n[workspaces]\nnames = [\"\", \"\", \"\", \"\", \"\", \"\", "
        "\"\", \"\", \"\"]   # blank = decimal index\n"
        "\n[keys]\n# bind = [\"combo action\", ...] — replaces defaults\n"
        "# defaults: super+Return terminal - alt+q close - alt+Tab switcher\n"
        "#   alt+space launcher - super+e settings - super+m quit\n"
        "#   super+Escape reload - alt+w wallpaper - alt+r shuffle\n"
        "#   super+s cycle layout - super+g float - super+f fullscreen\n"
        "#   super+grave state picker - super+p scratchpad\n"
        "#   super+shift+s scratch_mark\n"
        "#   alt+arrows directional focus (left/right/up/down)\n"
        "# actions: quit spawn_terminal close_focused cycle_layout\n"
        "#   toggle_float ratio_shrink ratio_grow nmaster_inc\n"
        "#   nmaster_dec view_ws N send_ws N toggle_prev_ws\n"
        "#   scratch_toggle scratch_mark ws_to_monitor reload\n"
        "#   restart wallpaper_random wallpaper_next\n"
        "#   wallpaper_pick menu_settings show_switcher\n"
        "#   show_launcher mru_step focus_left focus_right\n"
        "#   focus_up focus_down exec CMD (launch program/script)\n"
        "# example:\n# bind = [\"super+Return spawn_terminal\",\n"
        "#          \"super+m quit\",\n"
        "#          \"super+x exec firefox\"]\n"
        "\n[mouse]\nmodifier = \"super\"      # super | alt\n"
        "move_button = 1\nresize_button = 3\n"
        "\n[switcher]\nscope = \"all\"         # all | monitor\n"
        "\n[launcher]\nscan_path = true       # index $PATH executables\n"
        "# custom_dir = \"/home/user/.local/bin\"  # extra scripts beyond $PATH\n"
        "history_size = 20            # 0..500\n"
        "# default_module = \"web\"   # unmatched input goes to this module\n"
        "# entry = [\"Editor | emacs\", \"Files | thunar\"]  # \"Label | command\"\n"
        "# prefix modules: type \"name <args>\" in the launcher\n"
        "# [[launcher.module]]\n"
        "# name = \"web\"\n"
        "# desc = \"DuckDuckGo search\"\n"
        "# cmd = \"xdg-open \\\"https://duckduckgo.com/?q=%%s\\\"\"  # %%s = typed args\n"
        "\n[wallpaper]\n"
        "dirs = []                     # e.g. [\"~/Pictures/wallpapers\"]\n"
        "setter_command = \"feh --bg-scale %%s\"   # %%s = chosen path\n"
        "\n[autostart]\nrun = []        # e.g. [\"picom\", \"dunst\"]\n");
    fclose(f);
    return true;
}

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "conf.h"
#include "icons.h"
#include "launcher.h"
#include "menu.h"
#include "popup.h"
#include "settings.h"
#include "util.h"

/* §7.3: dmenu × prefix-module hybrid. Entries come from a
 * mtime-revalidated PATH scan, XDG applications directories' desktop
 * files, conf "Label | command" lines and [[launcher.module]] prefixes;
 * a frecency history (frequency × recency) sorts launched rows to the
 * top and persists outside austere.conf. Desktop entries resolve their
 * Icon= through the icon themes and draw it next to the row. */

#define MAX_ENTRIES 4096
#define ROW_MAX (MAX_ENTRIES + 1024)
#define MAX_HIST 2048
#define ICON_H 20

typedef struct {
    char *dir;
    time_t mt;
} ldir_t;

typedef struct {
    char **names; /* full paths, scan order */
    unsigned n;
    ldir_t dirs[64];
    unsigned ndirs;
} launcher_t;

static launcher_t L;

/* ---- frecency history ------------------------------------------------ */

typedef struct {
    char *cmd;
    unsigned count;
    time_t last; /* unix time of most recent launch; 0 = legacy line */
} hent_t;

static hent_t *hist_arr;
static unsigned nhist;

static void
hist_path(char *out, unsigned outsz)
{
    const char *xdg = getenv("XDG_DATA_HOME");

    if (xdg && *xdg)
        snprintf(out, outsz, "%s/austere/history", xdg);
    else
        snprintf(out, outsz, "%s/.local/share/austere/history",
            getenv("HOME") ? getenv("HOME") : "/root");
}

/* Recency bucketing multiplied by raw frequency: a command used twice
 * this hour outranks one used ten times a month ago. */
static long
hist_score_hent(const hent_t *h, time_t now)
{
    time_t age = h->last ? now - h->last : 86400L * 370;
    long mult = age < 3600 ? 5 : age < 86400 ? 3 : age < 604800 ? 2 : 1;

    return (long)h->count * mult;
}

static int
hist_cmp(const void *a, const void *b)
{
    const hent_t *ha = a, *hb = b;
    time_t now = time(NULL);
    long sa = hist_score_hent(ha, now), sb = hist_score_hent(hb, now);

    if (sa != sb)
        return sa > sb ? -1 : 1;
    if (ha->last != hb->last)
        return ha->last > hb->last ? -1 : 1;
    return strcmp(ha->cmd, hb->cmd);
}

static void
hist_load(void)
{
    for (unsigned i = 0; i < nhist; i++)
        free(hist_arr[i].cmd);
    free(hist_arr);
    hist_arr = NULL;
    nhist = 0;

    char path[512];

    hist_path(path, sizeof(path));
    FILE *f = fopen(path, "r");

    if (!f)
        return;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!n)
            continue;
        char *save = NULL;
        char *a = strtok_r(line, "\t", &save);
        char *b = a ? strtok_r(NULL, "\t", &save) : NULL;
        char *c = b ? strtok_r(NULL, "\t", &save) : NULL;
        hent_t h;

        if (c) { /* count \t last \t cmd (frecency era) */
            h.count = (unsigned)strtoul(a, NULL, 10);
            h.last = (time_t)strtoll(b, NULL, 10);
            h.cmd = xstrdup(c);
        } else { /* legacy bare cmd */
            h.count = 1;
            h.last = 0;
            h.cmd = xstrdup(a);
        }
        /* merge duplicates */
        unsigned i;

        for (i = 0; i < nhist; i++)
            if (!strcmp(hist_arr[i].cmd, h.cmd)) {
                hist_arr[i].count += h.count;
                if (h.last > hist_arr[i].last)
                    hist_arr[i].last = h.last;
                free(h.cmd);
                break;
            }
        if (i == nhist) {
            if (nhist == MAX_HIST) {
                free(h.cmd);
            } else {
                hent_t *nh = realloc(hist_arr,
                    (nhist + 1) * sizeof(hent_t));

                if (nh) {
                    hist_arr = nh;
                    hist_arr[nhist++] = h;
                } else {
                    free(h.cmd);
                }
            }
        }
    }
    fclose(f);
    qsort(hist_arr, nhist, sizeof(hent_t), hist_cmp);
}

static void
hist_save(void)
{
    char path[512];

    hist_path(path, sizeof(path));
    char *slash = strrchr(path, '/');

    if (slash) {
        *slash = '\0';
        mkdir_p(path); /* best effort */
        *slash = '/';
    }
    FILE *f = fopen(path, "w");

    if (!f)
        return;
    unsigned cap = cfg.launcher_history_size;

    for (unsigned i = 0; i < nhist && i < cap; i++)
        fprintf(f, "%u\t%lld\t%s\n", hist_arr[i].count,
            (long long)hist_arr[i].last, hist_arr[i].cmd);
    fclose(f);
}

/* Case-insensitive so a history entry recorded from a PATH basename
 * ("firefox") boosts the desktop row named "Firefox". */
static long
hist_score(const char *cmd, time_t now)
{
    for (unsigned i = 0; i < nhist; i++)
        if (!strcasecmp(hist_arr[i].cmd, cmd))
            return hist_score_hent(&hist_arr[i], now);
    return 0;
}

static void
hist_push(const char *cmd)
{
    if (!cmd || !*cmd)
        return;
    time_t now = time(NULL);

    for (unsigned i = 0; i < nhist; i++)
        if (!strcmp(hist_arr[i].cmd, cmd)) {
            hist_arr[i].count++;
            hist_arr[i].last = now;
            qsort(hist_arr, nhist, sizeof(hent_t), hist_cmp);
            hist_save();
            return;
        }
    if (nhist == MAX_HIST)
        return;
    hent_t *nh = realloc(hist_arr, (nhist + 1) * sizeof(hent_t));

    if (!nh)
        return;
    hist_arr = nh;
    hist_arr[nhist].cmd = xstrdup(cmd);
    hist_arr[nhist].count = 1;
    hist_arr[nhist].last = now;
    nhist++;
    qsort(hist_arr, nhist, sizeof(hent_t), hist_cmp);
    hist_save();
}

/* ---- PATH scan ------------------------------------------------------- */

static void
scan_dir(const char *dir, char **names, unsigned *n)
{
    DIR *d = opendir(dir);

    if (!d)
        return;
    struct dirent *e;

    /* full paths are stored: execvp on a bare name would search PATH
     * and miss custom_dir entries */
    while (*n < MAX_ENTRIES && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char full[1024];

        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (access(full, X_OK) == 0)
            names[(*n)++] = xstrdup(full);
    }
    closedir(d);
}

/* Rescan only when a PATH directory (or custom_dir) changed mtime. */
static void
scan_refresh(void)
{
    ldir_t cur[64];
    unsigned ncur = 0;
    const char *path = getenv("PATH");
    char buf[1024];
    char *save = NULL;
    char *tok = NULL;

    snprintf(buf, sizeof(buf), "%s", path ? path : "/usr/bin:/bin");
    if (cfg.launcher_scan_path)
        tok = strtok_r(buf, ":", &save);
    for (; tok && ncur < 64; tok = strtok_r(NULL, ":", &save)) {
        struct stat st;

        if (stat(tok, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;
        cur[ncur].dir = tok;
        cur[ncur].mt = st.st_mtime;
        ncur++;
    }
    if (cfg.launcher_custom_dir && ncur < 64) {
        struct stat st;

        if (stat(cfg.launcher_custom_dir, &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            /* custom entries first: they must not be crowded out by
             * a PATH scan that hits the entry cap */
            memmove(cur + 1, cur, ncur * sizeof(cur[0]));
            cur[0].dir = cfg.launcher_custom_dir;
            cur[0].mt = st.st_mtime;
            ncur++;
        }
    }

    bool stale = ncur != L.ndirs;

    for (unsigned i = 0; i < ncur && !stale; i++)
        if (!L.dirs[i].dir || strcmp(L.dirs[i].dir, cur[i].dir) ||
            L.dirs[i].mt != cur[i].mt)
            stale = true;
    if (stale) {
        for (unsigned i = 0; i < L.n; i++)
            free(L.names[i]);
        L.n = 0;
        if (!L.names)
            L.names = xmalloc(MAX_ENTRIES * sizeof(char *));
        for (unsigned i = 0; i < L.ndirs; i++) {
            free(L.dirs[i].dir);
            L.dirs[i].dir = NULL;
        }
        L.ndirs = 0;
        for (unsigned i = 0; i < ncur; i++) {
            L.dirs[L.ndirs].dir = xstrdup(cur[i].dir);
            L.dirs[L.ndirs].mt = cur[i].mt;
            L.ndirs++;
            scan_dir(cur[i].dir, L.names, &L.n);
        }
    }
}

/* ---- XDG data directories -------------------------------------------- */

/* xdg_data_dirs() lives in src/icons.c (shared with notifications). */


/* ---- .desktop entries ------------------------------------------------ */

typedef struct de {
    char *name;     /* Name= (frecency identity) */
    char *generic;  /* GenericName= */
    char *comment;  /* Comment= */
    char *label;    /* "Name - Comment" for the row */
    char *exec;     /* sanitized command line (Terminal= applied later) */
    char *execbase; /* basename of the command, dedupe vs PATH bins */
    char *icon;     /* Icon= name, NULL if none */
    bool term;      /* Terminal= */
    struct de *next;
} de_t;

static de_t *de_list;

/* Desktop-filename dedupe across data dirs (user dirs win). Reset at
 * the start of every scan; a system can carry well over a thousand
 * entries once flatpak exports are counted. */
#define DE_SEEN_MAX 4096
static char de_seen[DE_SEEN_MAX][128];
static unsigned nseen;

static bool
de_bool(const char *v)
{
    return v && (!strcasecmp(v, "true") || !strcasecmp(v, "1") ||
        !strcasecmp(v, "yes"));
}

/* Split an Exec= line into argv tokens, honoring double quotes. */
static char **
de_argv(const char *exec, unsigned *n_out)
{
    size_t cap = 8, n = 0;
    char **av = xmalloc(cap * sizeof(char *));
    char tok[1024];
    unsigned tl = 0;
    bool inq = false;

    for (const char *p = exec;; p++) {
        char c = *p;

        if (!c || (c == ' ' && !inq)) {
            if (tl) {
                tok[tl] = '\0';
                if (n + 1 >= cap) {
                    cap *= 2;
                    av = realloc(av, cap * sizeof(char *));
                }
                av[n++] = xstrdup(tok);
                tl = 0;
            }
            if (!c)
                break;
            continue;
        }
        if (c == '"') {
            inq = !inq;
            continue;
        }
        if (tl + 1 < sizeof(tok))
            tok[tl++] = c;
    }
    *n_out = (unsigned)n;
    return av;
}

static bool
arg_need_quote(const char *a)
{
    return strpbrk(a, " \t'\"\\$;&|<>`()*?[]#~") != NULL;
}

/* Sanitize Exec=: drop % field codes, @@..@@ file-forwarding blocks
 * and an env K=V... prefix; return a shell-ready command line. */
static char *
exec_make(const char *exec)
{
    unsigned n = 0;
    char **av = de_argv(exec, &n);

    /* keep[] is an explicit membership mask rather than a compacted
     * copy: shifting tokens down would let the cleanup pass free the
     * same pointer twice. calloc: skipped tokens keep no entry. */
    bool *keep = calloc(n ? n : 1, sizeof(bool));
    bool span = false; /* inside an @@..@@ block */

    if (!keep) {
        for (unsigned i = 0; i < n; i++)
            free(av[i]);
        free(av);
        return xstrdup("");
    }

    for (unsigned i = 0; i < n; i++) {
        const char *a = av[i];

        if (a[0] == '@' && a[1] == '@') {
            span = !span;
            continue;
        }
        if (span || a[0] == '%')
            continue; /* field code / file-forwarding marker */
        keep[i] = true;
    }
    if (n && keep[0] && !strcmp(av[0], "env")) {
        /* env VAR=VAL cmd...: keep only the eventual command */
        unsigned first = 1;

        while (first < n && (!keep[first] || strchr(av[first], '=')))
            first++;
        for (unsigned i = 0; i < first; i++)
            keep[i] = false;
    }
    char *out = xmalloc(strlen(exec) * 2 + 64);
    unsigned o = 0;
    bool first_arg = true;

    for (unsigned i = 0; i < n; i++) {
        const char *a = av[i];

        if (!keep[i])
            continue;
        if (!first_arg)
            out[o++] = ' ';
        first_arg = false;
        bool need = arg_need_quote(a);

        if (need)
            out[o++] = '\'';
        for (const char *s = a; *s; s++) {
            if (*s == '\'') { /* close, escaped quote, reopen */
                out[o++] = '\'';
                out[o++] = '\\';
                out[o++] = '\'';
            }
            out[o++] = *s;
        }
        if (need)
            out[o++] = '\'';
    }
    out[o] = '\0';
    for (unsigned i = 0; i < n; i++)
        free(av[i]);
    free(av);
    free(keep);
    return out;
}

/* Terminal=true entries run inside the configured terminal. */
static char *
exec_terminal(const char *cmd)
{
    const char *term = cfg.terminal && *cfg.terminal ? cfg.terminal : NULL;

    if (!term)
        return xstrdup(cmd);
    char *out = xmalloc(strlen(term) + strlen(cmd) + 12);

    sprintf(out, "%s -e %s", term, cmd);
    return out;
}

static bool
list_contains(const char *semicolon_list, const char *want)
{
    if (!semicolon_list || !*semicolon_list)
        return false;
    char *buf = xstrdup(semicolon_list);
    char *save = NULL;
    bool found = false;

    for (char *tok = strtok_r(buf, ";", &save); tok;
         tok = strtok_r(NULL, ";", &save))
        if (!strcasecmp(tok, want)) {
            found = true;
            break;
        }
    free(buf);
    return found;
}

static de_t *
de_parse(const char *path)
{
    FILE *f = fopen(path, "r");

    if (!f)
        return NULL;
    de_t *de = calloc(1, sizeof(*de));
    char line[2048];
    bool ok = true; /* Type defaults to Application */

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!n)
            continue;
        char *eq = strchr(line, '=');

        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        while (*key == ' ' || *key == '\t')
            key++;
        while (*val == ' ' || *val == '\t')
            val++;
        if (!strcmp(key, "Type")) {
            ok = !strcmp(val, "Application");
        } else if (ok && !strcmp(key, "Name") && !de->name) {
            de->name = xstrdup(val);
        } else if (ok && !strcmp(key, "GenericName") &&
                   !de->generic) {
            de->generic = xstrdup(val);
        } else if (ok && !strcmp(key, "Comment") && !de->comment) {
            de->comment = xstrdup(val);
        } else if (ok && !strcmp(key, "Exec") && !de->exec) {
            de->exec = exec_make(val);
            char *sp = strchr(de->exec, ' ');

            if (sp)
                *sp = '\0';
            char *slash = strrchr(de->exec, '/');

            de->execbase = xstrdup(slash ? slash + 1 : de->exec);
            if (sp)
                *sp = ' ';
        } else if (ok && !strcmp(key, "Icon") && !de->icon) {
            de->icon = xstrdup(val);
        } else if (ok && !strcmp(key, "Terminal")) {
            de->term = de_bool(val);
        } else if (ok && !strcmp(key, "NoDisplay")) {
            if (de_bool(val))
                ok = false;
        } else if (ok && !strcmp(key, "Hidden")) {
            if (de_bool(val))
                ok = false;
        } else if (ok && !strcmp(key, "OnlyShowIn")) {
            const char *desk = getenv("XDG_CURRENT_DESKTOP");

            if (!desk || !list_contains(val, desk))
                ok = false;
        } else if (ok && !strcmp(key, "NotShowIn")) {
            const char *desk = getenv("XDG_CURRENT_DESKTOP");

            if (desk && list_contains(val, desk))
                ok = false;
        }
    }
    fclose(f);
    if (!ok || !de->name || !de->exec) {
        free(de->name);
        free(de->generic);
        free(de->comment);
        free(de->exec);
        free(de->execbase);
        free(de->icon);
        free(de);
        return NULL;
    }
    /* display label: Name - Comment (or GenericName), trimmed */
    const char *desc = de->comment && *de->comment
        ? de->comment
        : (de->generic && *de->generic ? de->generic : NULL);
    char buf[320];

    if (desc)
        snprintf(buf, sizeof(buf), "%s - %.200s", de->name, desc);
    else
        snprintf(buf, sizeof(buf), "%s", de->name);
    de->label = xstrdup(buf);
    return de;
}

static void
de_scan(void)
{
    char dirs[XDG_DIRS_MAX][XDG_DIR_LEN];
    unsigned nd = 0;

    nseen = 0; /* the dedupe set is per-scan, not per-session */
    xdg_data_dirs(dirs, &nd);
    for (unsigned d = 0; d < nd; d++) {
        char apath[640];

        snprintf(apath, sizeof(apath), "%s/applications", dirs[d]);
        DIR *dir = opendir(apath);

        if (!dir)
            continue;
        struct dirent *e;

        while (nseen < DE_SEEN_MAX && (e = readdir(dir))) {
            size_t nl = strlen(e->d_name);

            if (nl < 9 || nl > 120 ||
                strcmp(e->d_name + nl - 8, ".desktop"))
                continue;
            bool dup = false;

            for (unsigned i = 0; i < nseen; i++)
                if (!strcmp(de_seen[i], e->d_name)) {
                    dup = true;
                    break;
                }
            if (dup)
                continue;
            snprintf(de_seen[nseen], 128, "%.127s", e->d_name);
            nseen++;
            char full[1024];

            snprintf(full, sizeof(full), "%s/%.240s", apath, e->d_name);
            de_t *de = de_parse(full);

            if (de) {
                de->next = de_list;
                de_list = de;
            }
        }
        closedir(dir);
    }
}

static void
de_free_all(void)
{
    de_t *d = de_list;

    while (d) {
        de_t *nx = d->next;

        free(d->name);
        free(d->generic);
        free(d->comment);
        free(d->label);
        free(d->exec);
        free(d->execbase);
        free(d->icon);
        free(d);
        d = nx;
    }
    de_list = NULL;
}

/* Icon lookup (Icon= names, scaled to the row height) lives in
 * src/icons.c, shared with the notification toasts. */


/* ---- rows ------------------------------------------------------------ */

typedef struct {
    de_t *de;    /* desktop entry; launch via its Exec */
    char *label; /* row text */
    char *cmd;   /* explicit command (conf entry), NULL otherwise */
    char *key;   /* frecency identity, NULL if not tracked */
} row_t;

static row_t *rows;
static unsigned rows_n;

static void
row_add(de_t *de, const char *label, const char *cmd, const char *key)
{
    if (rows_n >= ROW_MAX)
        return;
    rows[rows_n].de = de;
    rows[rows_n].label = label ? xstrdup(label) : NULL;
    rows[rows_n].cmd = cmd ? xstrdup(cmd) : NULL;
    rows[rows_n].key = key ? xstrdup(key) : NULL;
    rows_n++;
}

static void
free_rows(void)
{
    for (unsigned i = 0; i < rows_n; i++) {
        free(rows[i].label);
        free(rows[i].cmd);
        free(rows[i].key);
    }
    free(rows);
    rows = NULL;
    rows_n = 0;
}

/* Does any launchable source already represent this history command?
 * Matched case-insensitively against conf entry labels, module names,
 * desktop names/exec basenames and PATH basenames. Such a history entry
 * is not repeated as its own row: the row it names carries the score. */
static bool
base_has_key(const char *cmd)
{
    for (unsigned i = 0; i < cfg.nlauncher_entries; i++) {
        const char *e = cfg.launcher_entries[i];
        const char *bar = strstr(e, " | ");

        if (!bar)
            continue;
        if ((size_t)(bar - e) == strlen(cmd) &&
            !strncasecmp(e, cmd, (size_t)(bar - e)))
            return true;
    }
    for (unsigned i = 0; i < cfg.nmodules; i++)
        if (!strcasecmp(cfg.mod_name[i], cmd))
            return true;
    for (de_t *d = de_list; d; d = d->next)
        if ((d->name && !strcasecmp(d->name, cmd)) ||
            (d->execbase && !strcasecmp(d->execbase, cmd)))
            return true;
    for (unsigned i = 0; i < L.n; i++) {
        const char *b = strrchr(L.names[i], '/');

        b = b ? b + 1 : L.names[i];
        if (!strcasecmp(b, cmd))
            return true;
    }
    return false;
}

typedef struct {
    unsigned idx;
    long score;
} scpair_t;

static int
sc_cmp(const void *a, const void *b)
{
    const scpair_t *x = a, *y = b;

    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return strcmp(rows[x->idx].label ? rows[x->idx].label : "",
        rows[y->idx].label ? rows[y->idx].label : "");
}

/* Frecency ranking: every launched row floats to the top of the list,
 * best score first; the never-launched tail keeps builder order. */
static void
rank_rows(void)
{
    if (rows_n < 2)
        return;
    scpair_t *sc = xmalloc(rows_n * sizeof(scpair_t));
    unsigned nsc = 0;
    time_t now = time(NULL);

    for (unsigned i = 0; i < rows_n; i++) {
        long s = rows[i].key ? hist_score(rows[i].key, now) : 0;

        if (s > 0) {
            sc[nsc].idx = i;
            sc[nsc].score = s;
            nsc++;
        }
    }
    if (!nsc) {
        free(sc);
        return; /* nothing launched yet: keep builder order */
    }
    qsort(sc, nsc, sizeof(scpair_t), sc_cmp);
    row_t *tmp = xmalloc(rows_n * sizeof(row_t));
    unsigned w = 0;

    for (unsigned i = 0; i < nsc; i++)
        tmp[w++] = rows[sc[i].idx];
    for (unsigned i = 0; i < rows_n; i++) {
        bool boosted = false;

        for (unsigned j = 0; j < nsc; j++)
            if (sc[j].idx == i) {
                boosted = true;
                break;
            }
        if (!boosted)
            tmp[w++] = rows[i];
    }
    memcpy(rows, tmp, rows_n * sizeof(row_t));
    free(tmp);
    free(sc);
}

static bool
path_row_taken(const char *base)
{
    for (de_t *d = de_list; d; d = d->next)
        if (d->execbase && !strcasecmp(d->execbase, base))
            return true;
    return false;
}

/* ---- module expansion ------------------------------------------------ */

/* Run a module template with %s replaced; returns malloc'd command.
 * Sized for one args splice per %s occurrence (never assume shrinkage). */
static char *
module_expand(unsigned mi, const char *args)
{
    const char *tmpl = cfg.mod_cmd[mi];
    const size_t alen = strlen(args);
    unsigned ns = 0;

    for (const char *p = tmpl; *p; p++)
        if (p[0] == '%' && p[1] == 's') {
            ns++;
            p++;
        }
    char *cmd = xmalloc(strlen(tmpl) + ns * alen + 1);
    unsigned o = 0;

    for (const char *p = tmpl; *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            memcpy(cmd + o, args, alen);
            o += (unsigned)alen;
            p++;
        } else {
            cmd[o++] = *p;
        }
    }
    cmd[o] = '\0';
    return cmd;
}

static bool
launcher_enter(wm_t *wm, const char *input, const char *row)
{
    /* 1. module prefix with args typed in the raw input */
    char pre[256];
    const char *args = strchr(input, ' ');

    if (args) {
        size_t plen = (size_t)(args - input);

        snprintf(pre, sizeof(pre), "%.*s", (int)plen, input);
        unsigned mi = launcher_module_find(pre);

        if (mi != UINT_MAX) {
            char *cmd = module_expand(mi, args + 1);

            spawn_shell(cmd);
            free(cmd);
            hist_push(input);
            return false;
        }
    }

    if (row) {
        for (unsigned i = 0; i < rows_n; i++) {
            if (!rows[i].label || strcmp(rows[i].label, row))
                continue;

            /* desktop entry: launch its sanitized Exec */
            if (rows[i].de) {
                char *cmd = exec_terminal(rows[i].de->exec);

                spawn_shell(cmd);
                free(cmd);
                hist_push(rows[i].de->name);
                return false;
            }
            /* conf labeled entry */
            if (rows[i].cmd) {
                spawn_shell(rows[i].cmd);
                hist_push(rows[i].label);
                return false;
            }
            /* bare module row: insert the prefix, keep browsing */
            if (rows[i].key) {
                for (unsigned mi = 0; mi < cfg.nmodules; mi++)
                    if (!strcmp(rows[i].key, cfg.mod_name[mi])) {
                        snprintf(pre, sizeof(pre), "%s ",
                            cfg.mod_name[mi]);
                        panel_set_input(pre);
                        return true;
                    }
                /* PATH binary row */
                for (unsigned k = 0; k < L.n; k++) {
                    const char *bn = strrchr(L.names[k], '/');

                    bn = bn ? bn + 1 : L.names[k];
                    if (!strcmp(bn, rows[i].key)) {
                        spawn_async((char *const[]){
                            L.names[k], NULL });
                        hist_push(rows[i].key);
                        return false;
                    }
                }
            }
            /* history row unfound in the current pools: re-route as
             * fresh input ("mark hi") */
            if (strcmp(rows[i].label, input) != 0)
                return launcher_enter(wm, rows[i].label, NULL);
            break;
        }
    }

    /* typed input naming a PATH binary with no row selected */
    if (*input && !row) {
        for (unsigned k = 0; k < L.n; k++) {
            const char *bn = strrchr(L.names[k], '/');

            bn = bn ? bn + 1 : L.names[k];
            if (!strcmp(bn, input)) {
                spawn_async((char *const[]){ L.names[k], NULL });
                hist_push(input);
                return false;
            }
        }
    }

    /* 6. default module takes the whole input */
    if (*input && cfg.launcher_default_module) {
        unsigned di = launcher_module_find(cfg.launcher_default_module);

        if (di != UINT_MAX) {
            char *cmd = module_expand(di, input);

            spawn_shell(cmd);
            free(cmd);
            hist_push(input);
            return false;
        }
    }
    popup_notify(wm, "launch: no match");
    return false;
}

/* ---- panel callbacks --------------------------------------------------- */

static image_t *
launcher_row_icon(wm_t *wm, unsigned rowidx)
{
    (void)wm;

    if (rowidx >= rows_n || !rows[rowidx].de)
        return NULL;
    return icon_get(rows[rowidx].de->icon, ICON_H);
}

static void
launcher_close(wm_t *wm)
{
    (void)wm;
    free_rows();
    de_free_all();
    /* release the scan cache too: RSS budget; the mtime validation
     * makes the next open's rescan cheap */
    for (unsigned i = 0; i < L.n; i++)
        free(L.names[i]);
    free(L.names);
    L.names = NULL;
    L.n = 0;
    /* drop the dir cache too, or the mtime check would skip the
     * rescan and the next open would list nothing */
    for (unsigned i = 0; i < L.ndirs; i++) {
        free(L.dirs[i].dir);
        L.dirs[i].dir = NULL;
    }
    L.ndirs = 0;
    for (unsigned i = 0; i < nhist; i++)
        free(hist_arr[i].cmd);
    free(hist_arr);
    hist_arr = NULL;
    nhist = 0;
    malloc_trim(0);
}

static char **labels;

void
launcher_open(wm_t *wm)
{
    scan_refresh();
    hist_load();
    free_rows();
    de_free_all();
    if (cfg.launcher_desktop)
        de_scan();

    rows = xmalloc(ROW_MAX * sizeof(row_t));
    rows_n = 0;

    /* history first, frecency-ordered; commands a launchable row
     * already covers are left to that row's score */
    for (unsigned i = 0; i < nhist; i++) {
        if (base_has_key(hist_arr[i].cmd))
            continue;
        row_add(NULL, hist_arr[i].cmd, NULL, hist_arr[i].cmd);
        if (rows_n == ROW_MAX)
            break;
    }

    /* conf entries: label on the row, command kept for Enter */
    for (unsigned i = 0; i < cfg.nlauncher_entries; i++) {
        char buf[256];

        snprintf(buf, sizeof(buf), "%s", cfg.launcher_entries[i]);
        char *bar = strstr(buf, " | ");

        if (!bar)
            continue;
        *bar = '\0';
        row_add(NULL, buf, bar + 3, buf);
    }

    /* prefix modules: "name - desc" */
    for (unsigned i = 0; i < cfg.nmodules; i++) {
        char buf[256];

        snprintf(buf, sizeof(buf), "%s - %s", cfg.mod_name[i],
            cfg.mod_desc[i] ? cfg.mod_desc[i] : "");
        row_add(NULL, buf, NULL, cfg.mod_name[i]);
    }

    /* desktop entries: friendly names, icons and frecency */
    for (de_t *d = de_list; d; d = d->next) {
        row_add(d, d->label, NULL, d->name);
        if (rows_n == ROW_MAX)
            break;
    }

    /* PATH binaries: rows show the basename; desktop entries launching
     * the same command win the slot */
    for (unsigned i = 0; i < L.n && rows_n < ROW_MAX; i++) {
        const char *base = strrchr(L.names[i], '/');

        base = base ? base + 1 : L.names[i];
        if (path_row_taken(base))
            continue;
        row_add(NULL, base, NULL, base);
    }
    rank_rows();

    free(labels);
    labels = xmalloc((rows_n ? rows_n : 1) * sizeof(char *));

    for (unsigned i = 0; i < rows_n; i++)
        labels[i] = rows[i].label;

    panel_def_t def = {
        .title = "launch",
        .prompt = "",
        .rows = labels,
        .nrows = rows_n,
        .filter = true,
        .tab_complete = true,
        .row_icon = launcher_row_icon,
        .on_enter = launcher_enter,
        .on_close = launcher_close,
    };

    panel_open(wm, &def);
}
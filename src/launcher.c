#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "launcher.h"
#include "menu.h"
#include "popup.h"
#include "settings.h"
#include "util.h"

/* §7.3: dmenu × prefix-module hybrid. Entries come from a
 * mtime-revalidated PATH scan, conf "Label | command" lines, and
 * [[launcher.module]] prefixes; history sorts successful launches to
 * the top and persists outside austere.conf. */

#define MAX_ENTRIES 4096

typedef struct {
    char *dir;
    time_t mt;
} ldir_t;

typedef struct {
    char **names;
    unsigned n;
    ldir_t dirs[64];
    unsigned ndirs;
    char **hist; /* most recent first */
    unsigned nhist;
} launcher_t;

static launcher_t L;

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

static void
hist_load(void)
{
    for (unsigned i = 0; i < L.nhist; i++)
        free(L.hist[i]);
    free(L.hist);
    L.hist = NULL;
    L.nhist = 0;

    char path[512];

    hist_path(path, sizeof(path));
    FILE *f = fopen(path, "r");

    if (!f)
        return;
    char line[512];
    unsigned cap = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!n)
            continue;
        if (L.nhist == cap) {
            cap = cap ? cap * 2 : 16;
            char **nh = realloc(L.hist, cap * sizeof(char *));

            if (!nh)
                break;
            L.hist = nh;
        }
        L.hist[L.nhist++] = xstrdup(line);
    }
    fclose(f);
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

    for (unsigned i = 0; i < L.nhist && i < cap; i++)
        fprintf(f, "%s\n", L.hist[i]);
    fclose(f);
}

static void
hist_push(const char *cmd)
{
    /* dedupe: move to front */
    for (unsigned i = 0; i < L.nhist; i++) {
        if (!strcmp(L.hist[i], cmd)) {
            free(L.hist[i]);
            memmove(L.hist + i, L.hist + i + 1,
                (L.nhist - i - 1) * sizeof(char *));
            L.nhist--;
            break;
        }
    }
    char **nh = realloc(L.hist, (L.nhist + 1) * sizeof(char *));

    if (!nh)
        return;
    L.hist = nh;
    memmove(L.hist + 1, L.hist, L.nhist * sizeof(char *));
    L.hist[0] = xstrdup(cmd);
    L.nhist++;
    hist_save();
}

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
    if (cfg.launcher_scan_path == false && ncur == 0) {
        /* scan disabled entirely */
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

/* ---- panel callbacks ------------------------------------------------ */

static char **rows_mem;
static char **row_cmds; /* command for labeled-entry rows, else NULL */
static unsigned rows_n;

static void
free_rows(void)
{
    for (unsigned i = 0; i < rows_n; i++) {
        free(rows_mem[i]);
        free(row_cmds[i]);
    }
    free(rows_mem);
    free(row_cmds);
    rows_mem = NULL;
    row_cmds = NULL;
    rows_n = 0;
}

/* Run a module template with %s replaced; returns malloc'd command. */
static char *
module_expand(unsigned mi, const char *args)
{
    const char *tmpl = cfg.mod_cmd[mi];
    char *cmd = xmalloc(strlen(tmpl) + strlen(args) + 1);
    unsigned o = 0;

    for (const char *p = tmpl; *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            o += (unsigned)sprintf(cmd + o, "%s", args);
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

    /* 2. a selected module row without args: insert prefix, keep
     * browsing (otter-style) */
    if (row) {
        for (unsigned i = 0; i < cfg.nmodules; i++) {
            size_t nl = strlen(cfg.mod_name[i]);

            if (strncmp(row, cfg.mod_name[i], nl) == 0 &&
                (row[nl] == '\0' ||
                    (row[nl] == ' ' && row[nl + 1] == '-' ))) {
                snprintf(pre, sizeof(pre), "%s ", cfg.mod_name[i]);
                panel_set_input(pre);
                return true;
            }
        }
    }

    /* 3. selected labeled entry: run its stored command */
    if (row) {
        for (unsigned i = 0; i < rows_n; i++)
            if (rows_mem[i] && !strcmp(rows_mem[i], row) &&
                row_cmds[i]) {
                spawn_shell(row_cmds[i]);
                hist_push(row);
                return false;
            }
    }

    /* 4. selected row or input is a PATH binary: direct execvp */
    const char *bin = row ? row : input;

    if (*bin) {
        for (unsigned i = 0; i < L.n; i++) {
            const char *base = strrchr(L.names[i], '/');

            base = base ? base + 1 : L.names[i];
            if (!strcmp(base, bin)) {
                spawn_async((char *const[]){ L.names[i], NULL });
                hist_push(bin);
                return false;
            }
        }
    }

    /* 5. history rows re-route as fresh input ("mark hi") */
    if (row && strcmp(row, input) != 0)
        return launcher_enter(wm, row, NULL);

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

static void
launcher_close(wm_t *wm)
{
    (void)wm;
    free_rows();
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
    for (unsigned i = 0; i < L.nhist; i++)
        free(L.hist[i]);
    free(L.hist);
    L.hist = NULL;
    L.nhist = 0;
    malloc_trim(0);
}

void
launcher_open(wm_t *wm)
{
    scan_refresh();
    hist_load();
    free_rows();

    rows_mem = xmalloc((MAX_ENTRIES + 64) * sizeof(char *));
    row_cmds = xmalloc((MAX_ENTRIES + 64) * sizeof(char *));
    rows_n = 0;

    /* history first (most recent at top) */
    for (unsigned i = 0; i < L.nhist; i++) {
        rows_mem[rows_n] = xstrdup(L.hist[i]);
        row_cmds[rows_n] = NULL;
        rows_n++;
    }

    /* conf entries: label on the row, command kept for Enter */
    for (unsigned i = 0; i < cfg.nlauncher_entries; i++) {
        char buf[256];

        snprintf(buf, sizeof(buf), "%s", cfg.launcher_entries[i]);
        char *bar = strstr(buf, " | ");

        if (!bar)
            continue;
        *bar = '\0';
        rows_mem[rows_n] = xstrdup(buf);
        row_cmds[rows_n] = xstrdup(bar + 3);
        rows_n++;
    }

    /* prefix modules: "name - desc" */
    for (unsigned i = 0; i < cfg.nmodules; i++) {
        char buf[256];

        snprintf(buf, sizeof(buf), "%s - %s", cfg.mod_name[i],
            cfg.mod_desc[i] ? cfg.mod_desc[i] : "");
        rows_mem[rows_n] = xstrdup(buf);
        row_cmds[rows_n] = NULL;
        rows_n++;
    }

    /* PATH binaries: rows show the basename */
    for (unsigned i = 0; i < L.n && rows_n < MAX_ENTRIES + 63; i++) {
        const char *base = strrchr(L.names[i], '/');

        base = base ? base + 1 : L.names[i];
        rows_mem[rows_n] = xstrdup(base);
        row_cmds[rows_n] = NULL;
        rows_n++;
    }


    panel_def_t def = {
        .title = "launch",
        .prompt = "",
        .rows = rows_mem,
        .nrows = rows_n,
        .filter = true,
        .tab_complete = true,
        .on_enter = launcher_enter,
        .on_close = launcher_close,
    };

    panel_open(wm, &def);
}

/* Config states: complete .toml configs living in
 * $XDG_CONFIG_HOME/austere/states/. The picker lists them; loading one
 * runs the same transactional swap as an explicit reload. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "conf.h"
#include "menu.h"
#include "popup.h"
#include "settings.h"
#include "states.h"
#include "util.h"

#define STATES_MAX 128

static char *state_rows[STATES_MAX];
static unsigned nstate_rows;

static void
states_dir(char *out, unsigned sz)
{
    char conf[512];

    snprintf(conf, sizeof(conf), "%s", conf_path());
    char *slash = strrchr(conf, '/');

    if (slash)
        *slash = '\0';
    snprintf(out, sz, "%s/states", conf);
}

static bool
name_safe(const char *s)
{
    if (!*s || strlen(s) > 96)
        return false;
    for (const char *p = s; *p; p++)
        if (*p == '/' || (*p == '.' && p[1] == '.'))
            return false;
    return true;
}

/* Last picked state, remembered across boots as states/.last. */
static void
marker_path(char *out, unsigned sz)
{
    char dir[512];

    states_dir(dir, sizeof(dir));
    snprintf(out, sz, "%s/.last", dir);
}

static void
states_mark(const char *name)
{
    char path[512];
    FILE *f;

    marker_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%s\n", name);
    fclose(f);
}

/* Boot override: the marked state's file while it still exists. */
const char *
states_boot_override(void)
{
    static char ret[640];
    char mpath[512], name[128], dir[512];
    struct stat st;
    FILE *f;

    marker_path(mpath, sizeof(mpath));
    f = fopen(mpath, "r");
    if (!f)
        return NULL;
    if (!fgets(name, sizeof(name), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    name[strcspn(name, "\r\n")] = '\0';
    if (!name_safe(name)) {
        fprintf(stderr, " unsafe\n");
        return NULL;
    }
    states_dir(dir, sizeof(dir));
    if (snprintf(ret, sizeof(ret), "%s/%s.toml", dir, name) >=
        (int)sizeof(ret))
        return NULL;
    int ok = stat(ret, &st) == 0 && S_ISREG(st.st_mode);

    return ok ? ret : NULL;
}

bool
states_load(wm_t *wm, const char *name)
{
    char dir[512], path[640];

    if (!name || !name_safe(name)) {
        popup_notify(wm, "state not found: %s", name ? name : "");
        return false;
    }
    states_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.toml", dir, name);

    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        popup_notify(wm, "state not found: %s", name);
        return false;
    }
    settings_apply_file(wm, path, name);
    states_mark(name);
    return true;
}

static bool
states_enter(wm_t *wm, const char *input, const char *row)
{
    (void)input;

    if (row)
        states_load(wm, row);
    return false;
}

void
menu_states_open(wm_t *wm)
{
    char dir[512];

    states_dir(dir, sizeof(dir));

    for (unsigned i = 0; i < nstate_rows; i++)
        free(state_rows[i]);
    nstate_rows = 0;

    DIR *dp = opendir(dir);

    if (!dp) {
        popup_notify(wm, "no states dir: %s", dir);
        return;
    }
    struct dirent *e;

    while (nstate_rows < STATES_MAX && (e = readdir(dp))) {
        size_t len = strlen(e->d_name);

        if (len < 6 || strcmp(e->d_name + len - 5, ".toml"))
            continue;
        e->d_name[len - 5] = '\0';
        state_rows[nstate_rows++] = xstrdup(e->d_name);
    }
    closedir(dp);

    if (!nstate_rows) {
        popup_notify(wm, "no states in %s", dir);
        return;
    }
    sort_strs(state_rows, nstate_rows, true);

    panel_def_t def = {
        .title = "config states", .prompt = "",
        .rows = state_rows, .nrows = nstate_rows,
        .filter = true, .on_enter = states_enter,
        .px_w = 320, .anchor_bar = true
    };

    panel_open(wm, &def);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "menu.h"
#include "util.h"
#include "welcome.h"

static const char *const essentials[] = {
    "super+space         terminal (kitty)",
    "alt+space           launcher",
    "alt+Tab             window switcher",
    "super+e             settings menu",
    "super+Tab           cycle windows (MRU)",
    "alt+q               close window",
    "super+m             quit",
    NULL,
};

static bool
welcome_enter(wm_t *wm, const char *input, const char *row)
{
    (void)wm;
    (void)input;
    (void)row;
    return false;
}

static void
welcome_close(wm_t *wm)
{
    (void)wm;
}

void
welcome_maybe_show(wm_t *wm)
{
    char path[512];
    const char *xdg = getenv("XDG_DATA_HOME");

    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%s/austere/welcome", xdg);
    else
        snprintf(path, sizeof(path),
            "%s/.local/share/austere/welcome",
            getenv("HOME") ? getenv("HOME") : "/root");

    struct stat st;

    if (stat(path, &st) == 0)
        return;

    char dir[512];

    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');

    if (slash) {
        *slash = '\0';
        mkdir_p(dir);
    }
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f); /* marker first: never shown twice, even on crash */

    panel_def_t def = {
        .title = "welcome to austere",
        .prompt = "",
        .rows = (char **)essentials,
        .nrows = 7,
        .filter = false,
        .tab_complete = false,
        .on_enter = welcome_enter,
        .on_close = welcome_close,
    };

    panel_open(wm, &def);
}

/* Freedesktop .desktop scanning for the application menu. Entries are
 * bucketed into four fixed categories (internet, system, creative,
 * media); everything unmatched lands in a trailing "other" group so no
 * installed app is unreachable. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "apps.h"
#include "util.h"
#include "wm.h"

#define APPS_MAX 512

static struct {
    char *name;
    char *exec;
    unsigned cat; /* 0..3 = fixed buckets, 4 = other */
} apps[APPS_MAX];
static unsigned napps;
static bool scanned;

static const char *const CAT_NAMES[] = { "internet", "system",
    "creative", "media", "other" };

/* Categories that map into each bucket; first match wins per entry. */
static const char *const CAT_KEYS[][6] = {
    { "Internet", "Network", "WebBrowser", "Email", "InstantMessaging",
        "P2P" },
    { "System", "Settings", "Utilities", "FileManager",
        "TerminalEmulator", "Development" },
    { "Graphics", "ImageProcessing", "RasterGraphics", "VectorGraphics",
        "Photography", "Scanning" },
    { "AudioVideo", "Audio", "Video", "Player", "Music", "Game" },
};

int
app_category(unsigned i)
{
    return i < napps ? (int)apps[i].cat : 4;
}

const char *
app_name(unsigned i)
{
    return i < napps ? apps[i].name : "";
}

const char *
app_exec(unsigned i)
{
    return i < napps ? apps[i].exec : "";
}

unsigned
apps_count(void)
{
    return napps;
}

void
apps_rescan(void)
{
    for (unsigned i = 0; i < napps; i++) {
        free(apps[i].name);
        free(apps[i].exec);
    }
    napps = 0;

    static const char *const dirs[] = {
        "%s/.local/share/applications", "/usr/share/applications"
    };
    char home[512];
    const char *h = getenv("HOME");

    snprintf(home, sizeof(home), dirs[0], h ? h : "");
    const char *paths[] = { home, dirs[1] };

    for (unsigned d = 0; d < 2 && napps < APPS_MAX; d++) {
        DIR *dp = opendir(paths[d]);

        if (!dp)
            continue;
        struct dirent *e;

        while (napps < APPS_MAX && (e = readdir(dp))) {
            size_t len = strlen(e->d_name);

            if (len < 8 || strcmp(e->d_name + len - 8, ".desktop"))
                continue;
            char full[1024];

            snprintf(full, sizeof(full), "%s/%s", paths[d], e->d_name);
            FILE *f = fopen(full, "r");

            if (!f)
                continue;
            char line[512];
            bool in_desktop = false, keep = true;
            char *name = NULL, *exec = NULL;
            char cats[256] = "";

            while (fgets(line, sizeof(line), f)) {
                if (line[0] == '[') {
                    in_desktop = !strncmp(line, "[Desktop Entry]",
                        15);
                    continue;
                }
                if (!in_desktop)
                    continue;
                char *v = strchr(line, '=');

                if (!v)
                    continue;
                *v++ = '\0';
                size_t vl = strlen(v);

                if (vl && v[vl - 1] == '\n')
                    v[vl - 1] = '\0';
                if (!strcmp(line, "Name") && !name)
                    name = xstrdup(v);
                else if (!strcmp(line, "Exec") && !exec)
                    exec = xstrdup(v);
                else if (!strcmp(line, "Categories"))
                    snprintf(cats, sizeof(cats), "%s", v);
                else if (!strcmp(line, "NoDisplay") &&
                    !strcasecmp(v, "true"))
                    keep = false;
            }
            fclose(f);

            if (!keep || !name || !exec || !*exec) {
                free(name);
                free(exec);
                continue;
            }
            apps[napps].name = name;
            /* strip field codes (%f %F %u %U %d %D %n %N %i %c %k %v
             * %m) and trailing whitespace from Exec */
            char *o = exec;

            for (char *p = exec; *p; p++) {
                if (*p == '%' && p[1]) {
                    p++;
                    continue;
                }
                *o++ = *p;
            }
            *o = '\0';
            for (char *p = o; p > exec && p[-1] == ' ';)
                *--p = '\0';

            unsigned cat = 4;

            apps[napps].exec = exec;

            for (unsigned c = 0; c < 4 && cat == 4; c++)
                for (unsigned k = 0; k < 6; k++)
                    if (*CAT_KEYS[c][k] && strstr(cats,
                        CAT_KEYS[c][k])) {
                        cat = c;
                        break;
                    }
            apps[napps].cat = cat;
            napps++;
        }
        closedir(dp);
    }
    scanned = true;
}

const char *
app_category_name(unsigned cat)
{
    return cat < 5 ? CAT_NAMES[cat] : CAT_NAMES[4];
}

const char *
app_exec_for(const char *name)
{
    for (unsigned i = 0; i < napps; i++)
        if (!strcmp(apps[i].name, name))
            return apps[i].exec;
    return NULL;
}

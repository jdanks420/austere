#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "icons.h"
#include "util.h"

#ifndef AUSTERE_NO_IMLIB2
#include <Imlib2.h>
#endif

void
xdg_data_dirs(char dirs[][XDG_DIR_LEN], unsigned *n)
{
    unsigned nd = 0;
    const char *home = getenv("XDG_DATA_HOME");

    if (home && *home)
        snprintf(dirs[nd++], XDG_DIR_LEN, "%s", home);
    else
        snprintf(dirs[nd++], XDG_DIR_LEN, "%s/.local/share",
            getenv("HOME") ? getenv("HOME") : "/root");
    const char *s = getenv("XDG_DATA_DIRS");

    if (!s || !*s)
        s = "/usr/local/share:/usr/share";
    char buf[2048];
    char *save = NULL;

    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok_r(buf, ":", &save);
         tok && nd < XDG_DIRS_MAX; tok = strtok_r(NULL, ":", &save)) {
        if (!*tok || !strcmp(tok, dirs[0]))
            continue;
        snprintf(dirs[nd++], XDG_DIR_LEN, "%s", tok);
    }
    if (nd < XDG_DIRS_MAX &&
        access("/var/lib/flatpak/exports/share", R_OK) == 0)
        snprintf(dirs[nd++], XDG_DIR_LEN, "/var/lib/flatpak/exports/share");
    *n = nd;
}

#ifndef AUSTERE_NO_IMLIB2

/* Cache entry: the image_t lives on the heap so pointers handed to a
 * panel stay valid when the cache array itself grows. */
typedef struct {
    char *key;   /* "name@height" */
    image_t *img;
} ico_t;

static ico_t *cache;
static unsigned ncache;

static char *
path_find(const char *name)
{
    char dirs[XDG_DIRS_MAX][XDG_DIR_LEN];
    unsigned nd = 0;
    static const char *sizes[] = { "256x256", "128x128", "64x64",
        "48x48", "32x32", "22x22", "16x16", NULL };
    static const char *exts[] = { ".png", ".xpm", ".jpeg", ".jpg", NULL };
    static char path[1024];

    /* an absolute path is taken as-is (a notification's app_icon) */
    if (name[0] == '/') {
        if (access(name, R_OK) == 0) {
            snprintf(path, sizeof(path), "%s", name);
            return path;
        }
        return NULL;
    }
    xdg_data_dirs(dirs, &nd);
    for (unsigned d = 0; d < nd; d++) {
        for (unsigned s = 0; sizes[s]; s++)
            for (unsigned e = 0; exts[e]; e++) {
                snprintf(path, sizeof(path),
                    "%s/icons/hicolor/%s/apps/%s%s",
                    dirs[d], sizes[s], name, exts[e]);
                if (access(path, R_OK) == 0)
                    return path;
            }
        for (unsigned e = 0; exts[e]; e++) {
            snprintf(path, sizeof(path), "%s/pixmaps/%s%s",
                dirs[d], name, exts[e]);
            if (access(path, R_OK) == 0)
                return path;
        }
    }
    return NULL;
}

image_t *
icon_get(const char *name, unsigned target_h)
{
    if (!name || !*name || target_h == 0)
        return NULL;
    char key[320];

    snprintf(key, sizeof(key), "%s@%u", name, target_h);
    for (unsigned i = 0; i < ncache; i++)
        if (!strcmp(cache[i].key, key))
            return cache[i].img;
    ico_t *ne = realloc(cache, (ncache + 1) * sizeof(ico_t));

    if (!ne)
        return NULL;
    cache = ne;
    cache[ncache].key = xstrdup(key);
    cache[ncache].img = NULL;
    ico_t *ent = &cache[ncache];

    ncache++;
    char *path = path_find(name);

    if (!path)
        return NULL;
    Imlib_Image src = imlib_load_image(path);

    if (!src)
        return NULL;
    imlib_context_set_image(src);
    int iw = imlib_image_get_width();
    int ih = imlib_image_get_height();

    if (iw <= 0 || ih <= 0) {
        imlib_free_image();
        return NULL;
    }
    int tw = (int)((long)iw * (long)target_h / ih);

    if (tw < 1)
        tw = 1;
    Imlib_Image scaled = imlib_create_cropped_scaled_image(0, 0, iw, ih,
        tw, (int)target_h);

    imlib_free_image();
    if (!scaled)
        return NULL;
    imlib_context_set_image(scaled);
    uint32_t *data = imlib_image_get_data_for_reading_only();
    uint32_t *buf = malloc((size_t)tw * target_h * sizeof(uint32_t));

    if (buf) {
        memcpy(buf, data, (size_t)tw * target_h * sizeof(uint32_t));
        ent->img = calloc(1, sizeof(image_t));
        if (ent->img) {
            ent->img->argb = buf;
            ent->img->w = (unsigned)tw;
            ent->img->h = target_h;
        } else
            free(buf);
    }
    imlib_free_image();
    return ent->img;
}

#else /* AUSTERE_NO_IMLIB2 */

image_t *
icon_get(const char *name, unsigned target_h)
{
    (void)name;
    (void)target_h;
    return NULL;
}

#endif

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

/* The decode half, shared by every entry point: load a file that has
 * already been resolved and scale it to target_h. It always allocates a
 * fresh image_t, so it is safe for callers that must neither read nor
 * grow the process-lifetime cache. */
static image_t *
decode_path(const char *path, unsigned target_h)
{
    Imlib_Image src, scaled;
    uint32_t *data, *buf;
    image_t *img;
    int iw, ih, tw;

    src = imlib_load_image(path);

    if (!src)
        return NULL;
    imlib_context_set_image(src);
    iw = imlib_image_get_width();
    ih = imlib_image_get_height();

    if (iw <= 0 || ih <= 0) {
        imlib_free_image();
        return NULL;
    }
    tw = (int)((long)iw * (long)target_h / ih);

    if (tw < 1)
        tw = 1;
    scaled = imlib_create_cropped_scaled_image(0, 0, iw, ih, tw,
        (int)target_h);

    imlib_free_image();
    if (!scaled)
        return NULL;
    imlib_context_set_image(scaled);
    data = imlib_image_get_data_for_reading_only();
    buf = malloc((size_t)tw * target_h * sizeof(uint32_t));
    img = calloc(1, sizeof(image_t));
    if (buf && img) {
        memcpy(buf, data, (size_t)tw * target_h * sizeof(uint32_t));
        img->argb = buf;
        img->w = (unsigned)tw;
        img->h = target_h;
    } else {
        free(buf);
        free(img);
        img = NULL;
    }
    imlib_free_image();
    return img;
}

/* Resolve a name (or an absolute path) against the plain XDG layout and
 * decode it. Nothing is remembered, so this is the entry point for
 * callers that must not grow the process-lifetime cache. */
image_t *
icon_resolve(const char *name, unsigned target_h)
{
    char *path;

    if (!name || !*name || target_h == 0)
        return NULL;
    path = path_find(name);
    if (!path)
        return NULL;
    return decode_path(path, target_h);
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
    ent->img = icon_resolve(name, target_h);
    return ent->img;
}

/* ---- icon theme resolution (Phase 3) ---------------------------------
 *
 * icon_resolve_ex() resolves names that arrived over the bus, so every
 * step is bounded: fixed-size buffers throughout, no directory scanned
 * that a theme's own metadata did not declare, and the only thing kept
 * between calls is a parsed index.theme keyed by that theme's path. A
 * name is never remembered, so a client that sends a fresh name on
 * every signal cannot grow anything.
 *
 * Order of the search:
 *   1. every extra_path root, as a theme when it has an index.theme and
 *      as a plain icon directory when it does not (some ship both),
 *   2. each XDG data dir's hicolor, the freedesktop fallback theme,
 *   3. the pre-existing hicolor/apps and pixmaps lookup, unchanged.
 */
#define ICON_NAME_MAX 160      /* a longer name is a miss, not a truncation */
#define ICON_PATH_MAX 1024
#define ICON_ROOT_MAX 8        /* extra_path entries honoured */
#define ICON_ROOT_LEN 512
#define ICON_EXTRA_MAX 4096    /* the whole remote extra_path string */
#define ICON_SUBDIR_MAX 64     /* "512x512/stock/chart" and friends */
#define ICON_KEY_MAX 64
#define THEME_DIR_MAX 768      /* hicolor itself declares 649 */
#define THEME_INHERIT_MAX 8
#define THEME_DEPTH_MAX 8      /* an Inherits chain may not be deeper */
#define THEME_CACHE_MAX 16

/* One declared subdirectory of a theme, and the size it is drawn at.
 * `size` 0 means the theme said nothing, which ranks as "any size". */
typedef struct {
    char sub[ICON_SUBDIR_MAX];
    unsigned size;
} tdir_t;

/* A parsed index.theme. `pins` is the live reference count of a lookup
 * that is walking this theme right now, so eviction can never pull the
 * ground out from under an inherited theme still being searched. */
typedef struct {
    char dir[ICON_ROOT_LEN];
    tdir_t *dirs;                            /* exactly ndirs entries */
    unsigned ndirs;
    char inherit[THEME_INHERIT_MAX][ICON_KEY_MAX];
    unsigned ninherit;
    unsigned pins;
    bool loaded;
} theme_t;

static theme_t themes[THEME_CACHE_MAX];
static unsigned theme_victim;         /* round-robin eviction cursor */

/* A vector extension is only worth trying when the loader can read one.
 * If the decode of a found SVG fails, the search is repeated with vector
 * extensions suppressed, so a theme that ships only scalable artwork
 * cannot turn into a permanent miss on a build without librsvg. */
static bool reject_vector;

static const char *const RASTER_EXTS[] = {
    ".png", ".xpm", ".jpeg", ".jpg", ".svg", NULL
};
static const char *const VECTOR_EXTS[] = {
    ".svg", ".png", ".xpm", ".jpeg", ".jpg", NULL
};
static const char *const NO_VECTOR_EXTS[] = {
    ".png", ".xpm", ".jpeg", ".jpg", NULL
};

/* Size subdirectories a bare icon directory is likely to use, ranked by
 * distance to the target like a theme's declared ones. */
static const struct {
    const char *sub;
    unsigned size;
} STD_SIZES[] = {
    { "8x8", 8 }, { "16x16", 16 }, { "22x22", 22 }, { "24x24", 24 },
    { "32x32", 32 }, { "36x36", 36 }, { "48x48", 48 }, { "64x64", 64 },
    { "72x72", 72 }, { "96x96", 96 }, { "128x128", 128 }, { "192x192", 192 },
    { "256x256", 256 },
};
#define STD_COUNT (sizeof(STD_SIZES) / sizeof(STD_SIZES[0]))

/* The roots one lookup searches. Filled per call, so nothing survives
 * it. */
typedef struct {
    char extra[ICON_ROOT_MAX][ICON_ROOT_LEN];
    unsigned nextra;
    char xdg[XDG_DIRS_MAX][XDG_DIR_LEN];
    unsigned nxdg;
} search_t;

static search_t srch;

/* A remote name is a bare name, never a path: no separator, no "..", no
 * control byte, and short enough that every path built from it still
 * fits a fixed buffer. */
static bool
name_ok(const char *name)
{
    size_t n = strlen(name);

    if (!n || n >= ICON_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7f)
            return false;
        if (c == '/' || c == '\\')
            return false;
        if (c == '.' && i + 1 < n && name[i + 1] == '.')
            return false;
    }
    return true;
}

/* An absolute path keeps its separators, but not a "..", a control byte,
 * or a length that would overflow a path buffer. */
static bool
abspath_ok(const char *path)
{
    size_t n = strlen(path);

    if (n < 2 || n >= ICON_PATH_MAX || path[0] != '/')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];

        if (c < 0x20 || c == 0x7f)
            return false;
        if (c == '/' && i + 2 < n && path[i + 1] == '.' && path[i + 2] == '.')
            return false;
    }
    return true;
}

static bool
copy_path(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap)
        return false;
    memcpy(dst, src, n + 1);
    return true;
}

/* Build <a>/<b>/<c><ext>, or <a>/<c><ext> when b is NULL, and report
 * whether the result is readable. Overflow is a miss rather than a
 * truncated path: a name long enough to overflow has no usable home. */
static bool
probe_path(const char *a, const char *b, const char *c, const char *ext,
    char *out, size_t cap)
{
    int n = b ? snprintf(out, cap, "%s/%s/%s%s", a, b, c, ext)
              : snprintf(out, cap, "%s/%s%s", a, c, ext);

    return n > 0 && (size_t)n < cap && access(out, R_OK) == 0;
}

/* Join path components into dst, reporting whether they all fit. */
static bool
join(char *dst, size_t cap, const char *a, const char *b, const char *c)
{
    int n = c ? snprintf(dst, cap, "%s/%s/%s", a, b, c)
              : snprintf(dst, cap, "%s/%s", a, b);

    return n > 0 && (size_t)n < cap;
}

/* ---- index.theme ------------------------------------------------------
 *
 * The format is INI-like, but a Directories= line can be several
 * kilobytes long (hicolor's is ~11 kB on one line), so the file is read
 * as a character stream and a comma list is cut into tokens as it
 * arrives: a long list costs one token buffer, not one line buffer.
 */
typedef enum {
    TS_LINE,     /* start of a line */
    TS_COMMENT,  /* inside a comment */
    TS_SECT,     /* inside [section] */
    TS_TAIL,     /* after ], ignoring the rest of the line */
    TS_KEY,      /* inside the key */
    TS_VAL,      /* inside the value, cut on ',' when it is a list */
} tstate_t;

typedef struct {
    tdir_t *dirs;                 /* grown during the parse */
    unsigned ndirs, cap;
    char inherit[THEME_INHERIT_MAX][ICON_KEY_MAX];
    unsigned ninherit;
    char key[ICON_KEY_MAX];
    char sect[ICON_SUBDIR_MAX];
    char tok[ICON_SUBDIR_MAX];
    unsigned nsect, nkey, ntok, cursor;
    bool in_theme;                /* inside [Icon Theme] */
    bool list;                    /* the current key is a comma list */
    long cur;                     /* the declared dir a section sizes */
} tparse_t;

static bool
tparse_grow(tparse_t *p)
{
    unsigned ncap = p->cap ? p->cap * 2 : 32;
    tdir_t *nd;

    if (p->cap >= THEME_DIR_MAX)
        return false;
    if (ncap > THEME_DIR_MAX)
        ncap = THEME_DIR_MAX;
    nd = realloc(p->dirs, (size_t)ncap * sizeof(tdir_t));
    if (!nd)
        return false;
    p->dirs = nd;
    p->cap = ncap;
    return true;
}

static void
tparse_add_dir(tparse_t *p)
{
    if (!*p->tok)
        return;
    if (p->ndirs == p->cap && (!tparse_grow(p) || p->ndirs == p->cap))
        return;   /* declared more directories than the cap allows */
    /* "16x16@2/status" is 16 logical pixels, so the leading number of
     * the directory name is a good answer when the theme has no section
     * for it. strtoul stops at the 'x', and yields 0 for "scalable". */
    snprintf(p->dirs[p->ndirs].sub, ICON_SUBDIR_MAX, "%s", p->tok);
    p->dirs[p->ndirs].size = (unsigned)strtoul(p->tok, NULL, 10);
    p->ndirs++;
}

static void
tparse_add_inherit(tparse_t *p)
{
    if (!*p->tok || p->ninherit >= THEME_INHERIT_MAX)
        return;
    snprintf(p->inherit[p->ninherit], ICON_KEY_MAX, "%s", p->tok);
    p->ninherit++;
}

/* A section header names a declared directory; note which one, so its
 * Size= line can refine what the name implied. Themes list their
 * sections in the same order as Directories in practice, so try that
 * first and fall back to a scan only when it does not line up. */
static void
tparse_sect(tparse_t *p)
{
    p->cur = -1;
    p->in_theme = !strcmp(p->sect, "Icon Theme");
    if (!*p->sect)
        return;
    if (p->cursor < p->ndirs && !strcmp(p->dirs[p->cursor].sub, p->sect)) {
        p->cur = (long)p->cursor;
        p->cursor++;
        return;
    }
    for (unsigned i = 0; i < p->ndirs; i++)
        if (!strcmp(p->dirs[i].sub, p->sect)) {
            p->cur = (long)i;
            break;
        }
}

static void
tparse_emit(tparse_t *p)
{
    p->tok[p->ntok] = '\0';
    if (!p->list) {
        if (!strcmp(p->key, "Size") && p->cur >= 0)
            p->dirs[p->cur].size = (unsigned)strtoul(p->tok, NULL, 10);
        return;
    }
    if (!p->in_theme)
        return;
    if (!strcmp(p->key, "Inherits"))
        tparse_add_inherit(p);
    else if (!strcmp(p->key, "Directories"))
        tparse_add_dir(p);
}

/* Parse one theme's index.theme into th. A file we cannot open, or one
 * that declares nothing usable, leaves the slot with no directories: the
 * lookup then simply misses rather than reading a directory blind. */
static void
theme_parse(const char *index_path, theme_t *th)
{
    FILE *f = fopen(index_path, "r");
    tparse_t p;
    tstate_t st = TS_LINE;
    int c;

    if (!f)
        return;
    memset(&p, 0, sizeof(p));
    p.cur = -1;
    while ((c = getc(f)) != EOF) {
        switch (st) {
        case TS_LINE:
            if (c == '\n')
                break;
            if (c == '#' || c == ';') {
                st = TS_COMMENT;
                break;
            }
            if (c == '[') {
                p.nsect = 0;
                p.sect[0] = '\0';
                st = TS_SECT;
                break;
            }
            if (c == ' ' || c == '\t' || c == '\r')
                break;
            p.nkey = 0;
            p.key[0] = '\0';
            p.key[p.nkey++] = (char)c;
            p.key[p.nkey] = '\0';
            st = TS_KEY;
            break;
        case TS_COMMENT:
            if (c == '\n')
                st = TS_LINE;
            break;
        case TS_SECT:
            if (c == ']') {
                p.sect[p.nsect] = '\0';
                tparse_sect(&p);
                st = TS_TAIL;
            } else if (c == '\n')
                st = TS_LINE;
            else if (p.nsect + 1 < ICON_SUBDIR_MAX)
                p.sect[p.nsect++] = (char)c;
            break;
        case TS_TAIL:
            if (c == '\n')
                st = TS_LINE;
            break;
        case TS_KEY:
            if (c == '=') {
                /* tolerate space before the '=' */
                while (p.nkey && (p.key[p.nkey - 1] == ' ' ||
                        p.key[p.nkey - 1] == '\t'))
                    p.key[--p.nkey] = '\0';
                p.key[p.nkey] = '\0';
                p.ntok = 0;
                p.tok[0] = '\0';
                p.list = p.in_theme && (!strcmp(p.key, "Directories") ||
                    !strcmp(p.key, "Inherits"));
                st = TS_VAL;
            } else if (c == '\n')
                st = TS_LINE;
            else if (p.nkey + 1 < ICON_KEY_MAX) {
                /* kept terminated at all times, so a key shorter than
                 * the one before it cannot read into its tail */
                p.key[p.nkey++] = (char)c;
                p.key[p.nkey] = '\0';
            }
            break;
        case TS_VAL:
            if (p.list && c == ',') {
                tparse_emit(&p);
                p.ntok = 0;
                p.tok[0] = '\0';
                break;
            }
            if (c == '\n' || c == '\r') {
                /* a value too long for the token buffer keeps its tail
                 * being split, so a huge list still yields every token
                 * that fits */
                tparse_emit(&p);
                st = TS_LINE;
                break;
            }
            if (p.ntok + 1 < ICON_SUBDIR_MAX)
                p.tok[p.ntok++] = (char)c;
            break;
        }
    }
    if (st == TS_VAL)
        tparse_emit(&p);
    fclose(f);
    /* hand the exact-size buffer over, so the documented per-theme
     * bound is what a full hicolor actually costs */
    if (p.ndirs && p.cap > p.ndirs + 8) {
        tdir_t *sh = realloc(p.dirs, (size_t)p.ndirs * sizeof(tdir_t));

        if (sh)
            p.dirs = sh;
    }
    th->dirs = p.dirs;
    th->ndirs = p.ndirs;
    th->ninherit = p.ninherit;
    memcpy(th->inherit, p.inherit, sizeof(th->inherit));
}

/* The cached index for a theme directory, parsed on a miss and held
 * (pinned) until theme_release(). NULL when the directory has no
 * readable index.theme, when its path is not usable, or when all slots
 * are pinned - which the depth cap rules out, since a chain is at most
 * THEME_DEPTH_MAX deep. */
static theme_t *
theme_acquire(const char *dir)
{
    char index[ICON_PATH_MAX];

    if (!abspath_ok(dir) || !join(index, sizeof(index), dir, "index.theme",
            NULL) || access(index, R_OK) != 0)
        return NULL;
    for (unsigned i = 0; i < THEME_CACHE_MAX; i++)
        if (themes[i].loaded && !strcmp(themes[i].dir, dir)) {
            themes[i].pins++;
            return &themes[i];
        }
    for (unsigned k = 0; k < THEME_CACHE_MAX; k++) {
        unsigned i = (theme_victim + k) % THEME_CACHE_MAX;

        if (themes[i].pins)
            continue;
        /* bounded replacement: the next unpinned slot, round robin */
        theme_victim = (i + 1) % THEME_CACHE_MAX;
        free(themes[i].dirs);
        memset(&themes[i], 0, sizeof(themes[i]));
        if (!copy_path(themes[i].dir, sizeof(themes[i].dir), dir))
            return NULL;
        themes[i].pins = 1;
        theme_parse(index, &themes[i]);
        themes[i].loaded = true;
        return &themes[i];
    }
    return NULL;
}

static void
theme_release(theme_t *th)
{
    if (th && th->pins)
        th->pins--;
}

/* ---- searching ------------------------------------------------------- */

/* A directory's worth of candidates, ordered by how close each declared
 * size is to the target. Stable, so two directories of the same distance
 * keep the order the theme declared them in. */
typedef struct {
    unsigned dist;
    unsigned idx;
} order_t;

static void
order_by_size(order_t *o, const tdir_t *dirs, unsigned n, unsigned target)
{
    for (unsigned i = 0; i < n; i++) {
        order_t v;
        unsigned lo = 0, hi = i, mid;

        /* a directory with no declared size matches any target, and is
         * ranked as if it were 0px: smaller rasters win, scalable
         * artwork beats the much larger rasters */
        v.dist = dirs[i].size ? (dirs[i].size > target ?
            dirs[i].size - target : target - dirs[i].size) : target;
        v.idx = i;
        while (lo < hi) {
            mid = lo + (hi - lo) / 2;
            if (o[mid].dist <= v.dist)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (unsigned k = i; k > lo; k--)
            o[k] = o[k - 1];
        o[lo] = v;
    }
}

static const char *const *
raster_exts(void)
{
    return reject_vector ? NO_VECTOR_EXTS : RASTER_EXTS;
}

static const char *const *
exts_for(const char *sub)
{
    /* a scalable directory holds vectors and a raster one does not, so
     * the order follows the directory: a miss costs one stat, not five */
    if (!reject_vector && !strncmp(sub, "scalable", 8))
        return VECTOR_EXTS;
    return raster_exts();
}

/* Probe one theme's declared directories, closest size first. The order
 * array is sized for the worst-case theme and lives on the stack, so an
 * Inherits chain of THEME_DEPTH_MAX frames costs a few tens of kB: the
 * deepest a lookup can go is bounded, which is what makes that fine. */
static bool
theme_scan(theme_t *th, const char *name, unsigned target_h, char *out,
    size_t cap)
{
    order_t order[THEME_DIR_MAX];
    bool found = false;

    if (!th->ndirs)
        return false;
    order_by_size(order, th->dirs, th->ndirs, target_h);
    for (unsigned i = 0; i < th->ndirs && !found; i++) {
        const tdir_t *d = &th->dirs[order[i].idx];
        const char *const *exts = exts_for(d->sub);

        for (unsigned e = 0; exts[e] && !found; e++)
            found = probe_path(th->dir, d->sub, name, exts[e], out, cap);
    }
    return found;
}

/* A root with no index.theme is a plain application icon directory, and
 * some ship artwork straight in it. Both shapes are probed: a size
 * subdirectory, then the root itself. */
static bool
bare_find(const char *root, const char *name, unsigned target_h, char *out,
    size_t cap)
{
    tdir_t std[STD_COUNT];
    order_t order[STD_COUNT];
    bool found = false;

    for (unsigned i = 0; i < STD_COUNT; i++) {
        snprintf(std[i].sub, ICON_SUBDIR_MAX, "%s", STD_SIZES[i].sub);
        std[i].size = STD_SIZES[i].size;
    }
    order_by_size(order, std, STD_COUNT, target_h);
    for (unsigned i = 0; i < STD_COUNT && !found; i++) {
        const char *sub = std[order[i].idx].sub;
        const char *const *exts = exts_for(sub);

        for (unsigned e = 0; exts[e] && !found; e++)
            found = probe_path(root, sub, name, exts[e], out, cap);
    }
    {
        const char *const *exts = raster_exts();

        for (unsigned e = 0; exts[e] && !found; e++)
            found = probe_path(root, NULL, name, exts[e], out, cap);
    }
    return found;
}

static bool theme_find(const char *theme_dir, const char *name,
    unsigned target_h, theme_t *seen[], unsigned nseen, char *out,
    size_t cap);

/* What a theme inherits is resolved the way the theme itself was: next
 * to it under a root we were given, or in the icons/ tree of an XDG data
 * dir. */
static bool
theme_inherit(theme_t *th, const char *name, unsigned target_h,
    theme_t *seen[], unsigned nseen, char *out, size_t cap)
{
    for (unsigned i = 0; i < th->ninherit; i++) {
        char cand[ICON_ROOT_LEN];

        for (unsigned e = 0; e < srch.nextra; e++)
            if (join(cand, sizeof(cand), srch.extra[e], th->inherit[i], NULL)
                && theme_find(cand, name, target_h, seen, nseen, out, cap))
                return true;
        for (unsigned x = 0; x < srch.nxdg; x++)
            if (join(cand, sizeof(cand), srch.xdg[x], "icons",
                    th->inherit[i])
                && theme_find(cand, name, target_h, seen, nseen, out, cap))
                return true;
    }
    return false;
}

/* Look for a name in one theme, then in what it inherits. The depth cap
 * is the hard bound that makes the walk terminate; `seen` is what keeps
 * it from re-walking a cycle all the way up to that bound, which is
 * worth having because every level rescans a theme's directories. */
static bool
theme_find(const char *theme_dir, const char *name, unsigned target_h,
    theme_t *seen[], unsigned nseen, char *out, size_t cap)
{
    theme_t *th = theme_acquire(theme_dir);
    bool found = false;

    if (!th)
        return false;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == th) {
            theme_release(th);   /* already on the chain: a cycle */
            return false;
        }
    if (nseen >= THEME_DEPTH_MAX) {
        theme_release(th);
        return false;
    }
    seen[nseen] = th;
    found = theme_scan(th, name, target_h, out, cap);
    if (!found)
        found = theme_inherit(th, name, target_h, seen, nseen + 1, out,
            cap);
    theme_release(th);
    return found;
}

static void
srch_build(const char *extra_path)
{
    char buf[ICON_EXTRA_MAX];
    char *save = NULL, *tok;

    memset(&srch, 0, sizeof(srch));
    xdg_data_dirs(srch.xdg, &srch.nxdg);
    if (!extra_path || !*extra_path)
        return;
    /* the string is remote: bound the copy before any of it is used as
     * a path, and keep only absolute, traversal-free roots */
    if (snprintf(buf, sizeof(buf), "%s", extra_path) >= (int)sizeof(buf))
        return;
    for (tok = strtok_r(buf, ":", &save);
         tok && srch.nextra < ICON_ROOT_MAX;
         tok = strtok_r(NULL, ":", &save)) {
        if (tok[0] != '/' || !abspath_ok(tok))
            continue;
        if (!copy_path(srch.extra[srch.nextra], ICON_ROOT_LEN, tok))
            continue;
        srch.nextra++;
    }
}

static bool
resolve_ex_path(const char *name, unsigned target_h, const char *extra_path,
    char *out, size_t cap)
{
    theme_t *seen[THEME_DEPTH_MAX];
    bool found = false;

    srch_build(extra_path);
    for (unsigned e = 0; e < srch.nextra && !found; e++) {
        found = theme_find(srch.extra[e], name, target_h, seen, 0, out, cap);
        if (!found)
            found = bare_find(srch.extra[e], name, target_h, out, cap);
    }
    for (unsigned x = 0; x < srch.nxdg && !found; x++) {
        char theme[ICON_ROOT_LEN];

        if (join(theme, sizeof(theme), srch.xdg[x], "icons", "hicolor"))
            found = theme_find(theme, name, target_h, seen, 0, out, cap);
    }
    if (!found) {
        /* the pre-existing hicolor/apps and pixmaps lookup, unchanged */
        const char *p = path_find(name);

        found = p && copy_path(out, cap, p);
    }
    return found;
}

/* The image this resolver handed out last. It is the resolver's, and it
 * lives only until the next call, which is what lets a remote name be
 * resolved without anything being remembered per name. */
static image_t *last;

static void
free_last(void)
{
    if (!last)
        return;
    free(last->argb);
    free(last);
    last = NULL;
}

const image_t *
icon_resolve_ex(const char *name, unsigned target_h, const char *extra_path)
{
    char path[ICON_PATH_MAX];

    free_last();
    if (!name || !*name || target_h == 0)
        return NULL;
    if (name[0] == '/') {
        /* an absolute path is still an icon path, validated the same way */
        if (!abspath_ok(name) || !copy_path(path, sizeof(path), name) ||
            access(path, R_OK) != 0)
            return NULL;
    } else {
        if (!name_ok(name) || !resolve_ex_path(name, target_h, extra_path,
                path, sizeof(path)))
            return NULL;
    }
    last = decode_path(path, target_h);
    if (!last && !reject_vector) {
        /* the file found was an SVG the loader could not read: try the
         * same search once more, without vector extensions */
        reject_vector = true;
        if (resolve_ex_path(name, target_h, extra_path, path, sizeof(path)))
            last = decode_path(path, target_h);
        reject_vector = false;
    }
    return last;
}

#else /* AUSTERE_NO_IMLIB2 */

image_t *
icon_resolve(const char *name, unsigned target_h)
{
    (void)name;
    (void)target_h;
    return NULL;
}

image_t *
icon_get(const char *name, unsigned target_h)
{
    (void)name;
    (void)target_h;
    return NULL;
}

const image_t *
icon_resolve_ex(const char *name, unsigned target_h, const char *extra_path)
{
    (void)name;
    (void)target_h;
    (void)extra_path;
    return NULL;
}

#endif

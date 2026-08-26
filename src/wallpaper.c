#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "draw.h"
#include "menu.h"
#include "monitor.h"
#include "popup.h"
#include "settings.h"
#include "util.h"
#include "wallpaper.h"

#ifndef AUSTERE_NO_IMLIB2
#include <Imlib2.h>
#endif

#define WP_MAX 512

static char **pool;
static unsigned npool;
static long last_index;
#ifndef AUSTERE_NO_IMLIB2
static uint32_t **thumb_cache;
#endif

/* Rebuild the pool listing. Identical listings are a no-op so picker
 * reopen costs nothing; otherwise thumbnails whose path survived are
 * carried over instead of decoded again. */
static void
pool_scan(void)
{
    static char *tmp[WP_MAX];
    unsigned n = 0;

    for (unsigned d = 0; d < cfg.nwp_dirs && n < WP_MAX; d++) {
        const char *dir = cfg.wp_dirs[d];

        if (*dir == '~' && dir[1] == '/') {
            static char exp[512];
            const char *home = getenv("HOME");

            snprintf(exp, sizeof(exp), "%s/%s", home ? home : "", dir + 2);
            dir = exp;
        }
        DIR *dp = opendir(dir);

        if (!dp)
            continue;
        struct dirent *e;

        while (n < WP_MAX && (e = readdir(dp))) {
            if (e->d_name[0] == '.')
                continue;
            char full[1024];

            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;

            if (stat(full, &st) == 0 && S_ISREG(st.st_mode))
                tmp[n++] = xstrdup(full);
        }
        closedir(dp);
    }

    bool same = n == npool;

    for (unsigned i = 0; same && i < n; i++)
        if (strcmp(tmp[i], pool[i])) {
            same = false;
            break;
        }
    if (same) {
        for (unsigned i = 0; i < n; i++)
            free(tmp[i]);
        return;
    }

    if (!pool)
        pool = xmalloc(WP_MAX * sizeof(char *));

#ifndef AUSTERE_NO_IMLIB2
    uint32_t **nc = NULL;

    if (n) {
        nc = calloc(n, sizeof(*nc));
        for (unsigned j = 0; nc && j < n; j++)
            for (unsigned i = 0; i < npool; i++)
                if (!strcmp(tmp[j], pool[i])) {
                    if (thumb_cache) {
                        nc[j] = thumb_cache[i];
                        thumb_cache[i] = NULL;
                    }
                    break;
                }
    }
    for (unsigned i = 0; thumb_cache && i < npool; i++)
        free(thumb_cache[i]);
    free(thumb_cache);
    thumb_cache = nc;
#endif

    for (unsigned i = 0; i < npool; i++)
        free(pool[i]);
    if (n)
        memcpy(pool, tmp, n * sizeof(*pool));
    npool = n;
}

static void
wp_apply(wm_t *wm, const char *path)
{
    if (!cfg.wp_setter || !strstr(cfg.wp_setter, "%s")) {
        popup_notify(wm, "wallpaper: setter lacks %%s");
        return;
    }
    char *cmd = xmalloc(strlen(cfg.wp_setter) + strlen(path) + 1);
    unsigned o = 0;

    for (const char *p = cfg.wp_setter; *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            o += (unsigned)sprintf(cmd + o, "%s", path);
            p++;
        } else {
            cmd[o++] = *p;
        }
    }
    cmd[o] = '\0';
    spawn_shell(cmd);
    free(cmd);
    popup_notify(wm, "wallpaper: %s", strrchr(path, '/')
        ? strrchr(path, '/') + 1
        : path);
}

void
wallpaper_random(wm_t *wm)
{
    pool_scan();
    if (!npool) {
        popup_notify(wm, "wallpaper pool is empty");
        return;
    }
    last_index = (long)((unsigned)rand() % npool);
    wp_apply(wm, pool[last_index]);
}

void
wallpaper_next(wm_t *wm)
{
    pool_scan();
    if (!npool)
        return;
    /* sorted cycle */
    for (unsigned i = 0; i < npool; i++)
        for (unsigned j = i + 1; j < npool; j++)
            if (strcmp(pool[i], pool[j]) > 0) {
                char *t = pool[i];

                pool[i] = pool[j];
                pool[j] = t;
            }
    last_index = (last_index + 1) % (long)npool;
    wp_apply(wm, pool[last_index]);
}

void
wallpaper_set(wm_t *wm, const char *path)
{
    if (path && *path) {
        for (unsigned i = 0; i < npool; i++)
            if (!strcmp(pool[i], path))
                last_index = (long)i;
        wp_apply(wm, path);
    }
}

/* ---- picker --------------------------------------------------------- */

static xcb_window_t pick_win;
static draw_t pick_dc;
static unsigned cell_w = 192, cell_h = 108;
static unsigned cols;
static unsigned sel_cell;
static unsigned first_row;
static unsigned rows_visible;
static bool pick_partial;     /* nav key: repaint old+new cells only */
static unsigned pick_prev_cell;
#ifndef AUSTERE_NO_IMLIB2
/* Decode once into RAM, then blit: imlib's drawable rendering wants an
 * Xlib display we don't own (§2 single connection). */
static void
thumb_fill(unsigned i)
{
    if (!thumb_cache || thumb_cache[i])
        return;
    Imlib_Image img = imlib_load_image(pool[i]);

    if (!img)
        return;
    imlib_context_set_image(img);
    Imlib_Image scaled = imlib_create_cropped_scaled_image(0, 0,
        imlib_image_get_width(), imlib_image_get_height(),
        (int)cell_w, (int)cell_h);
    imlib_free_image();
    if (!scaled)
        return;
    imlib_context_set_image(scaled);
    uint32_t *src = imlib_image_get_data_for_reading_only();

    thumb_cache[i] = malloc(cell_w * cell_h * sizeof(uint32_t));
    if (thumb_cache[i])
        memcpy(thumb_cache[i], src, cell_w * cell_h * sizeof(uint32_t));
    imlib_free_image();
}
#endif

static void
draw_cell(wm_t *wm, unsigned i)
{
    unsigned row = i / cols - first_row;
    unsigned col = i % cols;
    int x = (int)(col * (cell_w + 8) + 8);
    int y = (int)(40 + row * (cell_h + 22));
    int bt = i == sel_cell ? 4 : 1;

    /* filled backing rect; the thumbnail covers everything but the
     * ring */
    draw_rect(wm, &pick_dc, x - bt, y - bt,
        (unsigned)(cell_w + 2 * bt), (unsigned)(cell_h + 2 * bt),
        i == sel_cell ? cfg.focus_color : 0x303040);

#ifndef AUSTERE_NO_IMLIB2
    thumb_fill(i);
    if (thumb_cache && thumb_cache[i])
        xcb_put_image(wm->conn,
            XCB_IMAGE_FORMAT_Z_PIXMAP, pick_win,
            pick_dc.gc, (uint16_t)cell_w, (uint16_t)cell_h,
            (int16_t)x, (int16_t)y, 0,
            wm->scr->root_depth,
            cell_w * cell_h * 4,
            (const uint8_t *)thumb_cache[i]);
#endif
    const char *base = strrchr(pool[i], '/');

    base = base ? base + 1 : pool[i];
    draw_text(wm, &pick_dc, draw_ui_font(wm), x,
        y + (int)cell_h + 14, base, (unsigned)strlen(base),
        i == sel_cell ? 0xffffff : 0x888888, 0x101018);
}

/* Wipe a cell's bbox back to panel background before repainting it. */
static void
clear_cell_area(wm_t *wm, unsigned i)
{
    unsigned row = i / cols - first_row;
    unsigned col = i % cols;
    int x = (int)(col * (cell_w + 8) + 8);
    int y = (int)(40 + row * (cell_h + 22));

    draw_rect(wm, &pick_dc, x - 4, y - 4, cell_w + 8,
        cell_h + 28, 0x101018);
}

static bool
cell_visible(unsigned i)
{
    unsigned r = i / cols;

    return r >= first_row && r < first_row + rows_visible;
}

static void
pick_draw(wm_t *wm)
{
    monitor_t *mon = focused_mon(wm);
    unsigned w = mon->geom.w, h = mon->geom.h;

    cols = w / (cell_w + 8);
    if (!cols)
        cols = 1;
    unsigned rows_vis = (h - 60) / (cell_h + 22);

    draw_rect(wm, &pick_dc, 0, 0, w, h, 0x101018);
    char hdr[96];

    snprintf(hdr, sizeof(hdr),
        "wallpaper - arrows move - Enter set - Esc close (%u items)",
        npool);
    draw_text(wm, &pick_dc, draw_ui_font(wm), 8, 16, hdr,
        (unsigned)strlen(hdr), 0x888888, 0x101018);

    if (sel_cell >= npool)
        sel_cell = npool - 1;
    if (sel_cell / cols < first_row)
        first_row = sel_cell / cols;
    if (sel_cell / cols >= first_row + rows_vis)
        first_row = sel_cell / cols - rows_vis + 1;

    /* Pure navigation between on-screen cells repaints just those two;
     * full-window clears flicker because put_image bursts exceed the
     * socket buffer. Exposes and scrolls fall through to the full pass. */
    if (pick_partial && cell_visible(sel_cell) &&
        cell_visible(pick_prev_cell)) {
        clear_cell_area(wm, pick_prev_cell);
        clear_cell_area(wm, sel_cell);
        draw_cell(wm, pick_prev_cell);
        draw_cell(wm, sel_cell);
        pick_partial = false;
        return;
    }
    pick_partial = false;


    for (unsigned i = first_row * cols;
        i < npool && i < (first_row + rows_vis) * cols; i++)
        draw_cell(wm, i);
}

/* Click selects, quick second click on the same cell sets. All
 * repainting happens here so idle clicks cause no full redraw. */
static bool
pick_button(wm_t *wm, int16_t ex, int16_t ey, uint8_t btn,
    xcb_timestamp_t t)
{
    static xcb_timestamp_t last_t;
    static unsigned last_cell = (unsigned)-1;

    if (btn != XCB_BUTTON_INDEX_1 || !npool)
        return true;
    if (ex < 8 || ey < 40)
        return true;
    unsigned cx = (unsigned)(ex - 8) / (cell_w + 8);
    unsigned cy = (unsigned)(ey - 40) / (cell_h + 22);

    if (cx >= cols)
        return true;
    unsigned i = (first_row + cy) * cols + cx;

    if (i >= npool)
        return true;

    if (i == sel_cell && last_cell == i &&
        (uint32_t)(t - last_t) <= 400) {
        wallpaper_set(wm, pool[i]);
        menu_pop_overlay(wm);
        return true;
    }
    last_cell = i;
    last_t = t;
    if (i == sel_cell)
        return true;
    pick_prev_cell = sel_cell;
    sel_cell = i;
    pick_partial = true;
    pick_draw(wm);
    return true;
}

static void
pick_close(wm_t *wm)
{
    xcb_destroy_window(wm->conn, pick_win);
    pick_win = XCB_NONE;
    (void)wm;
}

static bool
pick_key(wm_t *wm, xcb_keysym_t sym, unsigned state)
{
    (void)state;
    unsigned prev = sel_cell;

    switch (sym) {
    case 0xff52: /* Up */
        if (sel_cell >= cols)
            sel_cell -= cols;
        break;
    case 0xff54: /* Down */
        if (sel_cell + cols < npool)
            sel_cell += cols;
        break;
    case 0xff51: /* Left */
    case 'h':
        if (sel_cell)
            sel_cell--;
        break;
    case 0xff53: /* Right */
    case 'l':
        if (sel_cell + 1 < npool)
            sel_cell++;
        break;
    case 0xff55: /* Prior */
        sel_cell = sel_cell >= cols * 4 ? sel_cell - cols * 4 : 0;
        break;
    case 0xff56: /* Next */
        sel_cell = sel_cell + cols * 4 < npool ? sel_cell + cols * 4
                                               : npool - 1;
        break;
    case 0xff50: /* Home */
        sel_cell = 0;
        break;
    case 0xff57: /* End */
        sel_cell = npool - 1;
        break;
    case 0xff0d: /* Return */
        wallpaper_set(wm, pool[sel_cell]);
        menu_pop_overlay(wm);
        return false;
    default:
        return true; /* consumed, no redraw */
    }
    if (sel_cell != prev) {
        pick_prev_cell = prev;
        pick_partial = true;
    }
    return true;
}

void
wallpaper_pick(wm_t *wm)
{
    pool_scan();
    if (!npool) {
        popup_notify(wm, "wallpaper pool is empty");
        return;
    }
    monitor_t *mon = focused_mon(wm);

    pick_win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, pick_win,
        wm->scr->root, (int16_t)mon->geom.x, (int16_t)mon->geom.y,
        (uint16_t)mon->geom.w, (uint16_t)mon->geom.h, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_BACK_PIXEL |
            XCB_CW_EVENT_MASK,
        (uint32_t[]){ 0x101018, 1,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
                XCB_EVENT_MASK_BUTTON_PRESS });
    draw_setup(wm, &pick_dc, pick_win);
    xcb_map_window(wm->conn, pick_win);
    overlay_t o = { .win = pick_win,
        .draw = pick_draw,
        .key = pick_key,
        .button = pick_button,
        .close = pick_close };

    menu_push_overlay(wm, &o);
}

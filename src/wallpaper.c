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

static void
pool_scan(void)
{
    for (unsigned i = 0; i < npool; i++)
        free(pool[i]);
    npool = 0;
    if (!pool)
        pool = xmalloc(WP_MAX * sizeof(char *));

    for (unsigned d = 0; d < cfg.nwp_dirs && npool < WP_MAX; d++) {
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

        while (npool < WP_MAX && (e = readdir(dp))) {
            if (e->d_name[0] == '.')
                continue;
            char full[1024];

            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;

            if (stat(full, &st) == 0 && S_ISREG(st.st_mode))
                pool[npool++] = xstrdup(full);
        }
        closedir(dp);
    }
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

    for (unsigned i = first_row * cols;
        i < npool && i < (first_row + rows_vis) * cols; i++) {
        unsigned row = i / cols - first_row;
        unsigned col = i % cols;
        int x = (int)(col * (cell_w + 8) + 8);
        int y = (int)(40 + row * (cell_h + 22));

        if (i == sel_cell)
            draw_rect(wm, &pick_dc, x - 3, y - 3, cell_w + 6,
                cell_h + 6, cfg.focus_color);
        else
            draw_rect(wm, &pick_dc, x - 3, y - 3, cell_w + 6,
                cell_h + 6, 0x303040);

#ifndef AUSTERE_NO_IMLIB2
        /* decode + scale in RAM, then blit via xcb: imlib's drawable
         * rendering wants an Xlib display we don't own (§2 single
         * connection) */
        Imlib_Image img = imlib_load_image(pool[i]);

        if (img) {
            imlib_context_set_image(img);
            Imlib_Image scaled = imlib_create_cropped_scaled_image(0,
                0, imlib_image_get_width(), imlib_image_get_height(),
                (int)cell_w, (int)cell_h);
            imlib_free_image();

            if (scaled) {
                imlib_context_set_image(scaled);
                uint32_t *px = imlib_image_get_data_for_reading_only();

                if (px)
                    xcb_put_image(wm->conn,
                        XCB_IMAGE_FORMAT_Z_PIXMAP, pick_win,
                        pick_dc.gc, (uint16_t)cell_w, (uint16_t)cell_h,
                        (int16_t)x, (int16_t)y, 0,
                        wm->scr->root_depth,
                        cell_w * cell_h * 4, (const uint8_t *)px);
                imlib_context_set_image(scaled);
                imlib_free_image();
            }
        }
#endif
        const char *base = strrchr(pool[i], '/');

        base = base ? base + 1 : pool[i];
        draw_text(wm, &pick_dc, draw_ui_font(wm), x,
            y + (int)cell_h + 14, base, (unsigned)strlen(base),
            i == sel_cell ? 0xffffff : 0x888888, 0x101018);
    }
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
    unsigned rows_vis = 1;

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
    (void)rows_vis;
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
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS });
    draw_setup(wm, &pick_dc, pick_win);
    xcb_map_window(wm->conn, pick_win);
    overlay_t o = { .win = pick_win,
        .draw = pick_draw,
        .key = pick_key,
        .close = pick_close };

    menu_push_overlay(wm, &o);
}

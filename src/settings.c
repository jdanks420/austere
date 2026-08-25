#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "bar.h"
#include "client.h"
#include "conf.h"
#include "keys.h"
#include "layout.h"
#include "popup.h"
#include "settings.h"
#include "util.h"
#include "workspace.h"

settings_t cfg;

void
settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));

    s->terminal = xstrdup("kitty");
    s->socket = true;

    s->border_width = 2;
    s->focus_color = 0x5f819d;
    s->unfocus_color = 0x444444;
    s->urgent_color = 0xaf3f3f;
    s->gap = 0;
    s->smart_gaps = false;
    s->corner_radius = 0;
    s->font = NULL; /* draw.c falls back to embedded Agave */

    s->bar_bottom = false;
    s->time_format = xstrdup("%a %d %b %H:%M");
    s->bar_bg = 0x1a1a1a;
    s->bar_fg = 0xcccccc;

    s->focus_follows_mouse = true;
    s->raise_on_click = true;
    s->snap_distance = 12;
    s->popup_timeout = 5;
    s->swallowing = false;

    s->default_layout = xstrdup("tile");
    s->nmaster = 1;
    s->split_ratio = 0.5;
    s->ratio_step = 0.05;

    s->nbinds = 0; /* keys.c seeds compiled-in defaults */

    s->mouse_mods = XCB_MOD_MASK_4;
    s->move_button = 1;
    s->resize_button = 3;

    s->switcher_monitor_scope = false;

    s->launcher_scan_path = true;
    s->launcher_custom_dir = NULL;
    s->launcher_history_size = 20;
    s->launcher_default_module = NULL;
    s->launcher_entries = NULL;
    s->nlauncher_entries = 0;
    s->mod_name = s->mod_desc = s->mod_cmd = NULL;
    s->nmodules = 0;

    s->wp_dirs = NULL;
    s->nwp_dirs = 0;
    s->wp_setter = xstrdup("feh --bg-scale %s");
}

const char *wallpaper_current(void); /* wallpaper.c */

unsigned
launcher_module_find(const char *prefix)
{
    for (unsigned i = 0; i < cfg.nmodules; i++)
        if (!strcmp(cfg.mod_name[i], prefix))
            return i;
    return UINT_MAX;
}

const char *
launcher_module_name(unsigned idx)
{
    return idx < cfg.nmodules ? cfg.mod_name[idx] : NULL;
}

void
settings_free_strings(settings_t *s)
{
    free(s->terminal);
    free(s->font);
    free(s->time_format);
    free(s->default_layout);
    for (unsigned i = 0; i < WS_MAX; i++)
        free(s->ws_names[i]);
    memset(s->ws_names, 0, sizeof(s->ws_names));
    s->terminal = s->font = s->time_format = s->default_layout = NULL;
    free(s->launcher_custom_dir);
    free(s->launcher_default_module);
    s->launcher_custom_dir = s->launcher_default_module = NULL;
    for (unsigned i = 0; i < s->nlauncher_entries; i++)
        free(s->launcher_entries[i]);
    free(s->launcher_entries);
    s->launcher_entries = NULL;
    s->nlauncher_entries = 0;
    for (unsigned i = 0; i < s->nmodules; i++) {
        free(s->mod_name[i]);
        free(s->mod_desc[i]);
        free(s->mod_cmd[i]);
    }
    free(s->mod_name);
    free(s->mod_desc);
    free(s->mod_cmd);
    s->mod_name = s->mod_desc = s->mod_cmd = NULL;
    s->nmodules = 0;

    s->wp_dirs = NULL;
    s->nwp_dirs = 0;
    s->wp_setter = xstrdup("feh --bg-scale %s");
}

const char *wallpaper_current(void); /* wallpaper.c */

/* Take ownership of src's heap strings and bind array contents. */
void
settings_swap(settings_t *dst, settings_t *src)
{
    settings_free_strings(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

/* Transactional reload (§9.2): parse into scratch; any validation
 * failure keeps the running config untouched and surfaces a popup.
 * On success: swap, then re-apply everything the WM derives from
 * settings — grabs, borders, bar, workarea, layout params. Session
 * state (focus, floating geometry, per-ws ratios) is preserved. */
void
settings_reload(wm_t *wm)
{
    settings_t scratch;

    settings_defaults(&scratch);
    keys_defaults(&scratch);
    if (!conf_load(conf_path(), &scratch, true)) {
        fprintf(stderr, "austere: reload aborted (invalid conf)\n");
        popup_notify(wm, "reload: invalid conf, keeping previous");
        settings_free_strings(&scratch);
        return;
    }

    settings_swap(&cfg, &scratch);

    ungrab_keys(wm);
    grab_keys(wm);

    for (client_t *c = wm->clients; c; c = c->next) {
        xcb_configure_window(wm->conn, c->win,
            XCB_CONFIG_WINDOW_BORDER_WIDTH,
            (uint32_t[]){ cfg.border_width });
        set_border(wm, c, wm->focused == c ? cfg.focus_color
                                           : cfg.unfocus_color);
    }

    for (unsigned i = 0; i < WS_MAX; i++) {
        free(workspaces[i].name);
        workspaces[i].name = cfg.ws_names[i]
            ? xstrdup(cfg.ws_names[i])
            : NULL;
    }
    bars_sync(wm);
    arrange(wm);
    popup_notify(wm, "config reloaded");
}

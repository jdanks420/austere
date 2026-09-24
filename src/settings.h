#ifndef AUSTERE_SETTINGS_H
#define AUSTERE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "workspace.h"

/* Single source of truth for runtime configuration (SPEC §9). Every
 * field needs: struct entry, default, conf key, live-apply case
 * (§9.4); the menu row arrives with M8. */

#define MAX_BINDS 64
#define BIND_CMD_MAX 192

typedef struct {
    unsigned mods;
    xcb_keysym_t keysym;
    uint8_t action; /* action registry id (event.h) */
    int arg;        /* workspace index for ws actions, else -1 */
    /* command for ACT_EXEC binds (program/script to launch, run via
     * /bin/sh -c); inline so no heap ownership bookkeeping in swap */
    char cmd[BIND_CMD_MAX];
    /* keycodes resolved once at grab time; the KeyPress path matches
     * against these so dispatch never allocates or re-scans the
     * keysyms table */
    xcb_keycode_t codes[8];
    uint8_t ncodes;
} bind_t;

typedef struct {
    /* [general] */
    char *terminal;
    bool socket;

    /* [appearance] */
    unsigned border_width;
    uint32_t focus_color;
    uint32_t unfocus_color;
    uint32_t urgent_color;
    unsigned gap;
    bool smart_gaps;
    unsigned corner_radius;
    char *font;
    bool deco;
    unsigned deco_title_h;
    uint32_t deco_border;
    uint32_t deco_unfocus_border;
    /* [deco] buttons: minimize, maximize, close glyphs; NULL/"" keeps
     * the built-in nerd-font glyph */
    char *deco_buttons[3];

    /* [bar] */
    bool bar_bottom;
    char *time_format;
    uint32_t bar_bg;
    uint32_t bar_fg;
    unsigned bar_gap;

    /* [bar] module placement: three ordered groups; NULL/0 = defaults */
    char **bar_left;
    unsigned nbar_left;
    char **bar_center;
    unsigned nbar_center;
    char **bar_right;
    unsigned nbar_right;

    /* [behavior] */
    bool focus_follows_mouse;
    bool raise_on_click;
    unsigned snap_distance;
    unsigned popup_timeout;
    /* [notifications] */
    bool notify_enabled;     /* serve org.freedesktop.Notifications */
    unsigned notify_timeout; /* default toast life, seconds */
    unsigned notify_max;     /* toasts kept in the stack */
    bool swallowing;

    /* [layouts] */
    char *default_layout;
    unsigned nmaster;
    double split_ratio;
    double ratio_step;

    /* [workspaces] */
    char *ws_names[WS_MAX]; /* NULL entries use the decimal index */

    /* [keys] */
    bind_t binds[MAX_BINDS];
    unsigned nbinds;

    /* [mouse] */
    unsigned mouse_mods;
    unsigned move_button;
    unsigned resize_button;

    /* [switcher] */
    bool switcher_monitor_scope; /* all workspaces when false */
    bool switcher_live_preview;  /* alt+Tab swaps focus as the selection moves */

    /* [launcher] */
    bool launcher_scan_path;
    bool launcher_desktop; /* list .desktop applications (friendly names) */
    char *launcher_custom_dir;
    unsigned launcher_history_size;
    char *launcher_default_module;
    char **launcher_entries; /* "Label | command" */
    unsigned nlauncher_entries;

    /* [wallpaper] */
    char **wp_dirs;
    unsigned nwp_dirs;
    char **autostart_run;
    unsigned nautostart_run;
    char *wp_setter; /* "%s" = chosen path */

    /* [[launcher.module]] */
    char **mod_name; /* parallel arrays: name/desc/cmd per module */
    char **mod_desc;
    char **mod_cmd;
    unsigned nmodules;
} settings_t;

unsigned launcher_module_find(const char *prefix); /* index or UINT_MAX */

extern settings_t cfg; /* same single-instance precedent as workspaces[] */

void settings_defaults(settings_t *s);
void settings_free_strings(settings_t *s);
void settings_swap(settings_t *dst, settings_t *src);
void settings_reapply_clients(wm_t *wm);
/* Record which file the running config came from (state or austere.conf)
 * so reload re-reads that one. */
void settings_set_active_conf(const char *path);

const char *conf_path(void);

#endif

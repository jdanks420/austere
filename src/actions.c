#include <string.h>

#include "actions.h"

static const action_ent_t registry[] = {
    { "quit", ACT_QUIT },
    { "spawn_terminal", ACT_SPAWN_TERMINAL },
    { "close_focused", ACT_CLOSE_FOCUSED },
    { "cycle_layout", ACT_CYCLE_LAYOUT },
    { "toggle_float", ACT_TOGGLE_FLOAT },
    { "ratio_shrink", ACT_MWFACT_DEC },
    { "ratio_grow", ACT_MWFACT_INC },
    { "nmaster_inc", ACT_NMASTER_INC },
    { "nmaster_dec", ACT_NMASTER_DEC },
    { "view_ws", ACT_VIEW_WS }, /* +0..WS_MAX-1 at parse time */
    { "send_ws", ACT_SEND_WS },
    { "toggle_prev_ws", ACT_TOGGLE_PREV_WS },
    { "scratch_toggle", ACT_SCRATCH_TOGGLE },
    { "scratch_mark", ACT_SCRATCH_MARK },
    { "ws_to_monitor", ACT_WS_TO_NEXT_MON },
    { "reload", ACT_RELOAD },
    { "menu_settings", ACT_MENU_SETTINGS },
    { "menu_states", ACT_MENU_STATES },
    { "load_state", ACT_LOAD_STATE },
    { "show_switcher", ACT_SHOW_SWITCHER },
    { "show_launcher", ACT_SHOW_LAUNCHER },
    { "menu_apps", ACT_MENU_APPS },
    { "mru_step", ACT_MRU_STEP },
    { "restart", ACT_RESTART },
    { "wallpaper_random", ACT_WALLPAPER_RANDOM },
    { "wallpaper_next", ACT_WALLPAPER_NEXT },
    { "wallpaper_pick", ACT_WALLPAPER_PICK },
    { NULL, 0 },
};

uint8_t
action_lookup(const char *name)
{
    for (const action_ent_t *e = registry; e->name; e++)
        if (!strcmp(e->name, name))
            return e->id;
    return 255;
}

const char *
action_name(uint8_t id)
{
    for (const action_ent_t *e = registry; e->name; e++)
        if (e->id == id)
            return e->name;
    return "?";
}

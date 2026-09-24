#ifndef AUSTERE_EVENT_H
#define AUSTERE_EVENT_H

#include "wm.h"

void event_loop(wm_t *wm);

enum action_id {
    ACT_QUIT,
    ACT_SPAWN_TERMINAL,
    ACT_CLOSE_FOCUSED,
    ACT_CYCLE_LAYOUT,
    ACT_TOGGLE_FLOAT,
    ACT_MWFACT_DEC,
    ACT_MWFACT_INC,
    ACT_NMASTER_INC,
    ACT_NMASTER_DEC,
    ACT_VIEW_WS = 0x10, /* +0..WS_MAX-1 */
    ACT_SEND_WS = 0x20, /* +0..WS_MAX-1 */
    ACT_TOGGLE_PREV_WS = 0x30,
    ACT_VIEW_PREV_WS,  /* view_ws_prev: step to previous workspace (wraps) */
    ACT_VIEW_NEXT_WS,  /* view_ws_next: step to next workspace (wraps) */
    ACT_SCRATCH_TOGGLE,
    ACT_SCRATCH_MARK,
    ACT_WS_TO_NEXT_MON,
    ACT_RELOAD,
    ACT_MENU_SETTINGS,
    ACT_SHOW_SWITCHER,
    ACT_SHOW_LAUNCHER,
    ACT_MENU_APPS,
    ACT_MENU_STATES,
    ACT_LOAD_STATE,
    ACT_SET_LAYOUT,
    ACT_MRU_STEP,
    ACT_RESTART,
    ACT_WALLPAPER_RANDOM,
    ACT_WALLPAPER_NEXT,
    ACT_WALLPAPER_PICK,
    ACT_FULLSCREEN,
    ACT_MAXIMIZE,
    ACT_RESTORE_MINIMIZED,
    ACT_TOGGLE_DECO,
    ACT_FOCUS_LEFT,
    ACT_FOCUS_RIGHT,
    ACT_FOCUS_UP,
    ACT_FOCUS_DOWN,
    ACT_EXEC, /* +cmd in bind_t: launch a program/script via /bin/sh -c */
    ACT_VOL_RAISE,
    ACT_VOL_LOWER,
    ACT_VOL_MUTE,
    ACT_COUNT,
};

#endif

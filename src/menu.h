#ifndef AUSTERE_MENU_H
#define AUSTERE_MENU_H

#include <stdbool.h>

#include "wm.h"

/* Settings menu (SPEC §9.3): a native modal panel over draw.c
 * primitives. Input is grabbed while open; every other event queues
 * normally and arrange() never sees the window. */

/* Shared list panel (SPEC §7.1): prompt + filterable rows + wrap nav.
 * The settings menu and the switcher/launcher/welcome panels are all
 * built on this. */
typedef struct {
    const char *title;
    const char *prompt;
    char **rows;        /* owned copies */
    unsigned nrows;
    bool filter;        /* typing filters rows by substring */
    bool tab_complete;  /* Tab completes common prefix (launcher) */
    /* return true to keep the panel open (input may be replaced via
     * panel_set_input — bare module selection in the launcher) */
    bool (*on_enter)(wm_t *wm, const char *input, const char *row);
    void (*on_close)(wm_t *wm);
} panel_def_t;

void panel_open(wm_t *wm, const panel_def_t *def);
const char *panel_input(void);
void panel_set_input(const char *s);

/* Full-screen overlay (wallpaper grid): owns keys while active. */
typedef struct {
    xcb_window_t win;
    void (*draw)(wm_t *wm);
    bool (*key)(wm_t *wm, xcb_keysym_t sym, unsigned state);
    void (*close)(wm_t *wm);
} overlay_t;

void menu_push_overlay(wm_t *wm, const overlay_t *o);
void menu_pop_overlay(wm_t *wm);

void menu_open(wm_t *wm);
void menu_close(wm_t *wm);
bool menu_active(void);
bool menu_owns_window(xcb_window_t win);
void menu_key(wm_t *wm, xcb_key_press_event_t *ev);
void menu_expose(wm_t *wm);

#endif

#ifndef AUSTERE_MENU_H
#define AUSTERE_MENU_H

#include <stdbool.h>
#include <stdint.h>

#include "draw.h"
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
    bool hold_alt;      /* closing Alt release closes the panel */
    unsigned init_sel;  /* preselected row at open; live-previewed once */
    /* optional per-row icon lookup, keyed by row index; NULL rows draw
     * text-only. Returns a pointer that stays valid for the session. */
    image_t *(*row_icon)(wm_t *wm, unsigned rowidx);
    /* return true to keep the panel open (input may be replaced via
     * panel_set_input — bare module selection in the launcher) */
    bool (*on_enter)(wm_t *wm, const char *input, const char *row);
    /* called whenever the selection changes (live preview/switch) */
    void (*on_preview)(wm_t *wm, const char *row);
    void (*on_close)(wm_t *wm);
    unsigned px_w;   /* fixed width; 0 = centered 3/5 monitor */
    bool anchor_bar; /* hug the bar edge beside the logo, full height */
} panel_def_t;

void panel_open(wm_t *wm, const panel_def_t *def);
void panel_set_input(const char *s);

/* Full-screen overlay (wallpaper grid): owns keys while active. */
typedef struct {
    xcb_window_t win;
    void (*draw)(wm_t *wm);
    bool (*key)(wm_t *wm, xcb_keysym_t sym, unsigned state);
    void (*close)(wm_t *wm);
    /* window-relative coords; returns true if the click was consumed.
     * Repainting is the callback's job. */
    bool (*button)(wm_t *wm, int16_t x, int16_t y, uint8_t btn,
        xcb_timestamp_t t);
} overlay_t;

bool menu_panel_button(wm_t *wm, xcb_window_t win, int16_t y,
    uint8_t btn);
void menu_push_overlay(wm_t *wm, const overlay_t *o);
void menu_pop_overlay(wm_t *wm);
bool menu_overlay_button(wm_t *wm, xcb_window_t win, int16_t x,
    int16_t y, uint8_t btn, xcb_timestamp_t t);

void menu_open(wm_t *wm);
void menu_close(wm_t *wm);
void menu_window_gone(wm_t *wm, xcb_window_t w);
bool menu_active(void);
xcb_window_t menu_top_window(void); /* panel/overlay window, XCB_NONE if none */
void menu_bump(wm_t *wm); /* panel stays topmost over any raised client */
bool menu_owns_window(xcb_window_t win);
void menu_key(wm_t *wm, xcb_key_press_event_t *ev);
void menu_key_release(wm_t *wm, xcb_key_release_event_t *ev);
void menu_expose(wm_t *wm);

#endif

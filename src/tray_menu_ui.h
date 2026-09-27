#ifndef AUSTERE_TRAY_MENU_UI_H
#define AUSTERE_TRAY_MENU_UI_H

#include <stdbool.h>

#include "wm.h"

/* The tray item's own flat menu, drawn as a native popup (SPEC §7.6.1).
 *
 * Presentation only, like the bar module: the backend publishes the rows
 * through tray_menu_view() and resolves an activated row's id inside
 * tray_menu_click(). Nothing here allocates a label, frees a backend
 * pointer, or touches the bus - the popup is a pure function of the
 * published state, so it can be torn down and rebuilt at any moment
 * without a client noticing.
 *
 * The window is one override-redirect, input-enabled popup modelled on
 * popup.c, NOT the keyboard-grabbing settings panel: it takes no focus,
 * grabs nothing, and never blocks the event loop. That is why Escape is
 * routed here by the event loop instead of arriving on its own window.
 *
 * Wiring the event loop needs (see docs/SPEC.md §7.6.1):
 *   - tray_menu_ui_sync()      after tray_pump()/tray_tick(), and on
 *                               tray_render_pending() - the backend
 *                               publishes HIDDEN -> LOADING -> READY (or
 *                               FAILED) and only bumps epoch when the
 *                               published content changes, so sync is
 *                               cheap when nothing happened.
 *   - tray_menu_ui_button()    for every XCB_BUTTON_PRESS. Order against
 *                               bar_button() is not load-bearing: a press
 *                               on a bar window closes the menu and is
 *                               passed on, so clicking another tray icon
 *                               while a menu is open switches menus
 *                               instead of only dismissing.
 *   - tray_menu_ui_key()       ahead of the keybind table, for every
 *                               XCB_KEY_PRESS, while a menu is up.
 *   - tray_menu_ui_expose()    for every XCB_EXPOSE.
 *   - tray_menu_ui_owns_window() ahead of the client-decoration and
 *                               client paths, so a WM-owned window is never
 *                               mistaken for a client's.
 *   - tray_menu_ui_shutdown()  beside bars_shutdown()/tray_shutdown().
 */

void tray_menu_ui_sync(wm_t *wm);

/* Click routing. Inside the popup any button press is consumed; button 1
 * over an enabled, non-separator row calls tray_menu_click() with the
 * row's snapshot index. Outside, the press closes the menu and is
 * consumed, except on a bar window (see above). A disabled or separator
 * row swallows the press without activating and without closing. */
bool tray_menu_ui_button(wm_t *wm, xcb_button_press_event_t *ev);

/* Escape dismisses. Every other key is left to the window that owns it:
 * the popup holds no focus and no grab, so the client keeps its keys. */
bool tray_menu_ui_key(wm_t *wm, xcb_key_press_event_t *ev);

bool tray_menu_ui_expose(wm_t *wm, xcb_window_t win);
bool tray_menu_ui_owns_window(xcb_window_t win);

/* True while the popup window exists, whether or not the backend still
 * calls the state READY. */
bool tray_menu_ui_active(void);

void tray_menu_ui_shutdown(wm_t *wm);

#endif

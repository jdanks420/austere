#ifndef AUSTERE_TRAY_INT_H
#define AUSTERE_TRAY_INT_H

#include <stdbool.h>

#include <dbus/dbus.h>

/* Internal D-Bus seam between the tray backend (src/tray.c) and the
 * DBusMenu client (src/tray_menu.c), and the only place in the tree
 * where libdbus types may appear outside src/tray.c itself. src/tray.h
 * stays D-Bus free and compiles in every build variant, so nothing that
 * includes it may reach this file.
 *
 * Included only from the D-Bus half of src/tray.c and of
 * src/tray_menu.c: under AUSTERE_NO_DBUS neither half exists, and both
 * files keep their inert stubs on the far side of the #ifndef instead.
 *
 * There is no behaviour here. Every trayint_* wrapper is a thin,
 * non-blocking view of something src/tray.c already does, so the two
 * halves share one connection, one reply-tracking table and one set of
 * registered object paths instead of opening a second connection or
 * running a second dispatch loop. The traymenu_* hooks are the calls in
 * the other direction, into the menu client.
 */

/* Which tracked call a reply belongs to. This is src/tray.c's private
 * enum, made canonical: the first three are the values it uses today,
 * in its current order, and the menu kinds are appended. Its pending
 * table is sized from the kind count, so the implementation phase must
 * resize that table in step with any kind added here. */
enum {
    TRAYINT_PEND_NAME = 0,     /* RequestName for the watcher name */
    TRAYINT_PEND_HOST,         /* RegisterStatusNotifierHost */
    TRAYINT_PEND_PROPS,        /* one item's GetAll */
    TRAYINT_PEND_MENU_ABOUT,   /* AboutToShow */
    TRAYINT_PEND_MENU_LAYOUT,  /* GetLayout */
    TRAYINT_PEND_MENU_PROPS,   /* GetGroupProperties */
    TRAYINT_PEND_KINDS
};

/* The live session connection, or NULL when there is none. The caller
 * does not own it: never close, unref, or block on it. */
DBusConnection *trayint_bus(void);

/* Monotonic milliseconds, the clock every internal deadline in the
 * tray uses, so both halves time out against one reference. */
long long trayint_now_ms(void);

/* Send without waiting for an answer. Consumes msg either way, and is
 * never a blocking call. */
void trayint_send(DBusMessage *msg);

/* Send and track the reply under a kind above, so it is harvested
 * exactly once and offered to traymenu_dispatch() on arrival. The
 * serial is handed back so the caller can recognise its own reply, the
 * way the item table matches a GetAll. Returns false, having consumed
 * msg, when the call could not be sent or the tracking table is full;
 * the caller then retries from a later round instead of stalling. */
bool trayint_send_tracked(DBusMessage *msg, int kind, dbus_uint32_t *serial_out);

/* Register and unregister an object path on the shared vtable,
 * reference counted: many items share /StatusNotifierItem, and a menu
 * path is held for exactly as long as its menu is. */
void trayint_path_add(const char *path);
void trayint_path_drop(const char *path);

/* Fire-and-forget match rule, as the tray uses for its own signals: the
 * daemon's answer is discarded, so a menu's LayoutUpdated and
 * ItemsPropertiesUpdated subscriptions cost no tracked reply. */
void trayint_add_match(const char *rule);

/* ---- the other direction, into src/tray_menu.c ------------------------ */

/* Start the exchange for one item. service and path are the canonical
 * identity copied out of the item table before any bus traffic, so the
 * menu keeps talking to the same item even if the table moves under it.
 * Returns false when there is no usable bus, and the caller then falls
 * back to the item's own ContextMenu. */
bool traymenu_open(const char *service, const char *path, int root_x,
    int root_y);

/* Offer one message to the menu client: a method call, a signal, or the
 * reply of a tracked menu kind (matched by the serial the caller
 * recorded). True when the menu took it, so the dispatch can stop
 * there. Never blocks and never consumes msg. */
bool traymenu_dispatch(DBusMessage *msg);

/* Fire the menu's own deadlines, chiefly an AboutToShow or GetLayout
 * that was never answered. Called once per tray pump round, and from
 * the timeout tick when the menu armed a deadline of its own. */
void traymenu_round(void);

/* The item is gone: drop its menu and any exchange still in flight for
 * it, and report the current state so the popup can close itself. */
void traymenu_item_gone(const char *service, const char *path);

#endif

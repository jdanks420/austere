#define _POSIX_C_SOURCE 200809L

/* The com.canonical.dbusmenu client behind src/tray.h's flat menu
 * contract, and the only file that speaks the menu protocol.
 *
 * One level, one item, one exchange at a time. Nothing here blocks: an
 * open sends the root Event("opened"), a tracked AboutToShow(0) and then
 * a tracked GetLayout(0, -1, []), and every step of the answer arrives
 * later through traymenu_dispatch(). While the exchange is outstanding
 * the state is LOADING, which the popup treats as "nothing to draw yet":
 * the window is only created once READY carries rows.
 *
 * Who owns what:
 *   - src/tray.c owns the connection, the reply table and the object path
 *     registrations, and hands them out through src/tray_int.h. This file
 *     never opens a second connection, never dispatches, and never calls
 *     a blocking helper.
 *   - This file owns the menu model: the rows, their labels and pixels,
 *     the epoch, and the identity of the item whose menu is open.
 *   - The public tray_menu_open() is in src/tray.c, because turning a
 *     snapshot index into an identity needs the private item table. The
 *     other three public calls are here, next to the state they report.
 *
 * The object the menu lives on is the path the item advertised in its Menu
 * property, which is not always the item's own path: an Ayatana-style
 * indicator exports both on one object, while a standard KDE one keeps
 * its SNI at /StatusNotifierItem and its menu at /MenuBar. The request
 * therefore carries both, and every call here goes to the advertised one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "icons.h"
#include "tray.h"

#ifndef AUSTERE_NO_DBUS

#include "tray_int.h"

#define MENU_IFACE "com.canonical.dbusmenu"
#define SNI_IFACE  "org.kde.StatusNotifierItem"

#define TRAYMENU_SERVICE_MAX 160
#define TRAYMENU_PATH_MAX 192
#define TRAYMENU_THEME_MAX 512
#define TRAYMENU_TITLE_MAX 64
/* Row artwork is decoded at the size a menu row is drawn at. The item
 * strip's size does not reach here, and the popup scales from this. */
#define TRAYMENU_ICON_PX 16

/* Shorter than the tray's wakeup hint on purpose: the client must always
 * be the one that decides an exchange has failed, never the loop's
 * wakeup being late. */
#define TRAYMENU_TIMEOUT_MS 1500

/* How many layout children are looked at before giving up on a
 * pathological one. Far above TRAY_MENU_MAX_ROWS, and bounded so a menu
 * offering thousands of rows cannot cost a long scan. */
#define TRAYMENU_SCAN_MAX (TRAY_MENU_MAX_ROWS * 4)

typedef struct {
    tray_menu_state_t state;
    unsigned epoch;
    int anchor_x, anchor_y;

    char service[TRAYMENU_SERVICE_MAX];
    /* the item's own object path: its identity, and what death is
     * matched against */
    char path[TRAYMENU_PATH_MAX];
    /* the com.canonical.dbusmenu object the item advertised through its
     * Menu property, which every menu call and signal is addressed to. It
     * is registered for exactly as long as the menu is open, and it is
     * the same string as `path` only for an item that exports both on one
     * object. */
    char menu_path[TRAYMENU_PATH_MAX];
    char theme_path[TRAYMENU_THEME_MAX];  /* the item's IconThemePath */
    char title[TRAYMENU_TITLE_MAX];

    tray_menu_row_t rows[TRAY_MENU_MAX_ROWS];
    char labels[TRAY_MENU_MAX_ROWS][TRAY_MENU_LABEL_MAX];
    image_t imgs[TRAY_MENU_MAX_ROWS];
    uint32_t *px[TRAY_MENU_MAX_ROWS];
    unsigned nrows;

    /* The in-flight exchange. Each serial is paired with the generation
     * it was sent in, so an answer to a menu that has since been closed
     * or reopened is recognised as stale and dropped. The generation is
     * NOT the epoch: the epoch is the reader's invalidation token and
     * moves whenever the published content might have, including on a
     * LayoutUpdated that has not been applied yet, so using it to judge
     * staleness would throw away every answer that arrived after a
     * signal. */
    dbus_uint32_t serial_about;
    dbus_uint32_t serial_layout;
    unsigned gen;
    unsigned gen_about;
    unsigned gen_layout;
    long long deadline;

    bool opened;     /* the root Event("opened") is owed an Event("closed") */
    bool refetch;    /* LayoutUpdated arrived; one refetch is owed */
    bool matched;    /* the dbusmenu match rule has been sent */
} menu_t;

static menu_t menu;

/* ---- publishing ------------------------------------------------------ */

/* Every visible change to the model goes through here, so the epoch is
 * bumped exactly when, and only when, a reader's snapshot went stale. */
static void
publish(tray_menu_state_t state)
{
    menu.state = state;
    menu.epoch++;
}

static void
rows_clear(void)
{
    for (unsigned i = 0; i < TRAY_MENU_MAX_ROWS; i++) {
        free(menu.px[i]);
        menu.px[i] = NULL;
    }
    memset(menu.rows, 0, sizeof(menu.rows));
    menu.nrows = 0;
}

/* A new menu instance: every call still in flight belongs to the one
 * before it. */
static void
generation_bump(void)
{
    menu.gen++;
}

/* Forget the exchange. The pending calls themselves are not ours to
 * cancel: src/tray_int.h has no cancel hook, and a copy left in the reply
 * table is reclaimed there once it completes or once its slot expires, so
 * this is bounded without ever cancelling from the wrong context. */
static void
retire(void)
{
    menu.serial_about = 0;
    menu.serial_layout = 0;
    menu.deadline = 0;
    menu.refetch = false;
}

static void
identity_clear(void)
{
    /* the menu object goes first: the registration is on the menu path,
     * and the item's own path is released by the item table, not here */
    if (menu.menu_path[0])
        trayint_path_drop(menu.menu_path);
    memset(menu.service, 0, sizeof(menu.service));
    memset(menu.path, 0, sizeof(menu.path));
    memset(menu.menu_path, 0, sizeof(menu.menu_path));
    memset(menu.theme_path, 0, sizeof(menu.theme_path));
    memset(menu.title, 0, sizeof(menu.title));
    menu.anchor_x = 0;
    menu.anchor_y = 0;
    menu.opened = false;
}

/* ---- outgoing traffic ------------------------------------------------ */

/* The timestamp the protocol wants. It is advisory: nothing depends on it
 * being the wall clock, but sending the real time is free. */
static dbus_uint32_t
now_s(void)
{
    return (dbus_uint32_t)time(NULL);
}

/* Event(id, eventId, <int32 0>, timestamp). The data variant is an empty
 * int32, which is what libdbusmenu's own python binding sends; some
 * clients accept a uint32 instead, so this is the variant that has to
 * work. Fire and forget, like every other item call. */
static void
event_send(int32_t id, const char *event)
{
    DBusMessage *m;
    DBusMessageIter it, var;
    int32_t zero = 0;
    dbus_uint32_t ts = now_s();

    m = dbus_message_new_method_call(menu.service, menu.menu_path, MENU_IFACE,
        "Event");
    if (!m)
        return;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &event);
    /* A variant container needs the signature of what is inside it, and
     * libdbus asserts on a NULL one, so "i" is not optional here. It also
     * has to be closed: an unclosed container is not part of the
     * message's signature, and the timestamp after it would be dropped
     * too - which sends a malformed message and costs the connection. */
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "i", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &zero);
    dbus_message_iter_close_container(&it, &var);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &ts);
    trayint_send(m);
}

static bool
about_send(void)
{
    DBusMessage *m;
    DBusMessageIter it;
    int32_t id = 0;

    m = dbus_message_new_method_call(menu.service, menu.menu_path, MENU_IFACE,
        "AboutToShow");
    if (!m)
        return false;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &id);
    /* Any previous AboutToShow is finished with: either its answer already
     * cleared the serial, or src/tray.c reaped the unanswered pending and no
     * answer can arrive. Dropping the serial here means a late reply can
     * never be mistaken for the answer to the call being sent. */
    menu.serial_about = 0;
    if (!trayint_send_tracked(m, TRAYINT_PEND_MENU_ABOUT,
            &menu.serial_about)) {
        menu.serial_about = 0;
        return false;
    }
    menu.gen_about = menu.gen;
    return true;
}

static bool
layout_send(void)
{
    DBusMessage *m;
    DBusMessageIter it, arr;
    int32_t id = 0, depth = -1;

    m = dbus_message_new_method_call(menu.service, menu.menu_path, MENU_IFACE,
        "GetLayout");
    if (!m)
        return false;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &depth);
    /* no property names: take everything the menu wants to say. An empty
     * array still has to be closed, or it never reaches the signature and
     * the call arrives with the wrong one */
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_close_container(&it, &arr);
    /* as in about_send: a reaped layout call is over, so its serial goes
     * before a new one is recorded, and a late reply to it is then dropped
     * by the serial match instead of being applied to these rows */
    menu.serial_layout = 0;
    if (!trayint_send_tracked(m, TRAYINT_PEND_MENU_LAYOUT,
            &menu.serial_layout)) {
        menu.serial_layout = 0;
        return false;
    }
    menu.gen_layout = menu.gen;
    return true;
}

/* No internal menu is worth showing: the item's own ContextMenu is what
 * the spec means by a fallback, and it is sent from here so no caller has
 * to know the rules. Never blocks, and leaves the view in FAILED so the
 * popup draws nothing. */
static void
fallback(const char *why)
{
    DBusMessage *m;
    DBusMessageIter it;
    int32_t x = (int32_t)menu.anchor_x, y = (int32_t)menu.anchor_y;

    fprintf(stderr, "austere: tray: %s %s: %s, using the item's own menu\n",
        menu.service, menu.path, why);
    m = dbus_message_new_method_call(menu.service, menu.path, SNI_IFACE,
        "ContextMenu");
    if (m) {
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &x);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &y);
        trayint_send(m);
    }
    retire();
    publish(TRAY_MENU_FAILED);
}

/* ---- reading a layout ------------------------------------------------ */

static const char *
prop_str(DBusMessageIter *v)
{
    const char *s = NULL;
    int type = dbus_message_iter_get_arg_type(v);

    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        return NULL;
    dbus_message_iter_get_basic(v, &s);
    return s;
}

static bool
prop_bool(DBusMessageIter *v, bool *out)
{
    dbus_bool_t b = FALSE;

    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_BOOLEAN)
        return false;
    dbus_message_iter_get_basic(v, &b);
    *out = b ? true : false;
    return true;
}

static bool
prop_int(DBusMessageIter *v, int32_t *out)
{
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_INT32)
        return false;
    dbus_message_iter_get_basic(v, out);
    return true;
}

/* GTK mnemonics: "_Open" reads as Open and "__literal" as _literal, so the
 * popup never has to know the convention. */
static void
label_copy(char *dst, size_t cap, const char *src)
{
    size_t o = 0;

    if (cap < 2)
        return;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] != '_') {
            dst[o++] = src[i];
            continue;
        }
        if (src[i + 1] == '_') {
            dst[o++] = '_';
            i++;
        }
        /* a lone '_' is the marker and goes; one at the end of the string
         * takes the rest of the word with it, as GTK intends */
    }
    dst[o] = '\0';
}

/* One row. `props` is already positioned at the first entry of the row's
 * a{sv}, so it is walked as it stands: recursing again would descend into
 * that first entry and read a key string as if it were the dictionary.
 * Returns true when the row took a slot, so an invisible row or a submenu
 * is skipped without consuming one of the published rows. */
static bool
row_apply(int32_t id, DBusMessageIter *props)
{
    const char *label = NULL, *type = NULL, *icon = NULL, *toggle = NULL;
    bool enabled = true, visible = true, checked = false;
    tray_menu_rowtype_t rt = TRAY_MENU_ROW_NORMAL;
    unsigned slot = menu.nrows;
    DBusMessageIter e;

    e = *props;
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, v;
        const char *key = NULL;

        dbus_message_iter_recurse(&e, &ent);
        if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_STRING)
            break;
        dbus_message_iter_get_basic(&ent, &key);
        dbus_message_iter_next(&ent);
        if (key && dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&ent, &v);
            if (!strcmp(key, "label"))
                label = prop_str(&v);
            else if (!strcmp(key, "type"))
                type = prop_str(&v);
            else if (!strcmp(key, "icon-name"))
                icon = prop_str(&v);
            else if (!strcmp(key, "toggle-type"))
                toggle = prop_str(&v);
            else if (!strcmp(key, "enabled"))
                prop_bool(&v, &enabled);
            else if (!strcmp(key, "visible"))
                prop_bool(&v, &visible);
            else if (!strcmp(key, "toggle-state")) {
                int32_t st = 0;

                if (prop_int(&v, &st))
                    checked = st == 0;   /* 0 on, 1 off, -1 indeterminate */
            }
        }
        dbus_message_iter_next(&e);
    }
    if (!visible)
        return false;
    /* Only a leaf is published: a row that is neither standard nor a
     * separator carries its own children, and submenus are deferred. */
    if (type && *type) {
        if (!strcmp(type, "separator"))
            rt = TRAY_MENU_ROW_SEPARATOR;
        else if (strcmp(type, "standard"))
            return false;
    }
    if (toggle) {
        if (!strcmp(toggle, "radio"))
            rt = TRAY_MENU_ROW_RADIO;
        else if (!strcmp(toggle, "checkmark"))
            rt = TRAY_MENU_ROW_TOGGLE;
    }
    if (rt != TRAY_MENU_ROW_TOGGLE && rt != TRAY_MENU_ROW_RADIO)
        checked = false;
    if (slot >= TRAY_MENU_MAX_ROWS)
        return false;

    label_copy(menu.labels[slot], TRAY_MENU_LABEL_MAX, label ? label : "");
    menu.rows[slot].type = rt;
    menu.rows[slot].checked = checked;
    menu.rows[slot].enabled = enabled;
    menu.rows[slot].label = menu.labels[slot];
    menu.rows[slot].img = NULL;
    menu.rows[slot].id = (unsigned)id;
    menu.nrows = slot + 1;

    /* Resolved uncached, like every other name a client chose, and copied
     * straight out: the resolver owns its image only until its next call.
     * The item's own IconThemePath comes with the request, so a row icon
     * is looked for in the same theme the item's own artwork came from. */
    if (icon && *icon) {
        const char *theme = menu.theme_path[0] ? menu.theme_path : NULL;
        const image_t *src = icon_resolve_ex(icon, TRAYMENU_ICON_PX, theme);
        size_t n;

        if (src && src->argb && src->w && src->h) {
            n = (size_t)src->w * src->h * sizeof(uint32_t);
            menu.px[slot] = malloc(n);
            if (menu.px[slot]) {
                memcpy(menu.px[slot], src->argb, n);
                menu.imgs[slot].argb = menu.px[slot];
                menu.imgs[slot].w = src->w;
                menu.imgs[slot].h = src->h;
                menu.rows[slot].img = &menu.imgs[slot];
            }
        }
    }
    return true;
}

/* The children of one layout, read one level deep. `arr` is the layout's
 * third field, an av: each element is normally a VARIANT wrapping one
 * child (ia{sv}av) struct, and an element that is a bare struct is
 * accepted too so a client that skips the wrapper still works. A child
 * that carries its own children is never descended into, because
 * submenus are deferred. */
static void
children_apply(DBusMessageIter *arr)
{
    DBusMessageIter child, inner, props;
    unsigned scanned = 0;

    dbus_message_iter_recurse(arr, &child);
    while (scanned < TRAYMENU_SCAN_MAX) {
        int type = dbus_message_iter_get_arg_type(&child);
        DBusMessageIter el;

        if (type != DBUS_TYPE_VARIANT && type != DBUS_TYPE_STRUCT)
            break;
        el = child;
        if (type == DBUS_TYPE_VARIANT)
            dbus_message_iter_recurse(&child, &el);   /* unwrap the child */
        dbus_message_iter_recurse(&el, &inner);
        if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_INT32) {
            int32_t id = 0;

            dbus_message_iter_get_basic(&inner, &id);
            dbus_message_iter_next(&inner);
            if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&inner, &props);
                if (dbus_message_iter_get_arg_type(&props) ==
                        DBUS_TYPE_DICT_ENTRY)
                    row_apply(id, &props);
            }
        }
        dbus_message_iter_next(&child);
        scanned++;
    }
}

/* The root layout's own properties: the title, and - only for a client
 * that hands its children over in a property instead of in the struct's
 * third field - the children. As with row_apply, `dict` is already
 * positioned at the first entry. Those children are handed back rather
 * than applied, so that the canonical field is read first and this
 * fallback can only be used when it produced nothing. */
static void
root_props(DBusMessageIter *dict, DBusMessageIter *compat, bool *have_compat)
{
    DBusMessageIter e;

    *have_compat = false;
    e = *dict;
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, v;
        const char *key = NULL;

        dbus_message_iter_recurse(&e, &ent);
        if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_STRING)
            break;
        dbus_message_iter_get_basic(&ent, &key);
        dbus_message_iter_next(&ent);
        if (key && dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&ent, &v);

            if (!strcmp(key, "children")) {
                if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_ARRAY) {
                    *compat = v;
                    *have_compat = true;
                }
            } else if (!strcmp(key, "label")) {
                const char *label = prop_str(&v);

                if (label)
                    snprintf(menu.title, sizeof(menu.title), "%s", label);
            }
        }
        dbus_message_iter_next(&e);
    }
}

static void
layout_apply(DBusMessage *reply)
{
    DBusMessageIter in, top, props, compat;
    bool have_compat = false;

    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        fallback("GetLayout failed");
        return;
    }
    /* u revision, (ia{sv}av) layout */
    if (!dbus_message_iter_init(reply, &in) ||
        dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_UINT32) {
        fallback("GetLayout answered with nothing usable");
        return;
    }
    dbus_message_iter_next(&in);
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRUCT) {
        fallback("GetLayout answered with nothing usable");
        return;
    }
    rows_clear();
    memset(menu.title, 0, sizeof(menu.title));
    /* The layout is (id, properties, children): the rows are the
     * struct's THIRD field. Reading the properties and expecting a
     * "children" key there is what made every real client look empty. */
    dbus_message_iter_recurse(&in, &top);      /* i id */
    dbus_message_iter_next(&top);              /* a{sv} properties */
    if (dbus_message_iter_get_arg_type(&top) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&top, &props);
        if (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY)
            root_props(&props, &compat, &have_compat);
        dbus_message_iter_next(&top);          /* av children */
        if (dbus_message_iter_get_arg_type(&top) == DBUS_TYPE_ARRAY)
            children_apply(&top);
    }
    if (!menu.nrows && have_compat)            /* only as a fallback */
        children_apply(&compat);
    if (!menu.nrows) {
        fallback("the menu has no rows to show");
        return;
    }
    /* A LayoutUpdated that landed while this call was in flight is already
     * covered by the layout just applied, so the refetch it asked for is
     * dropped rather than sent as a second round trip. */
    menu.refetch = false;
    publish(TRAY_MENU_READY);
}

static void
about_apply(DBusMessage *reply)
{
    DBusMessageIter in;

    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        fallback("AboutToShow failed");
        return;
    }
    /* The answer is a boolean need_update, but an empty reply means the
     * same thing to us, and some clients send one. The layout is fetched
     * either way: that is the only way to learn the rows. */
    if (dbus_message_iter_init(reply, &in) &&
        dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t need = FALSE;

        dbus_message_iter_get_basic(&in, &need);
        (void)need;
    }
    /* AboutToShow may have changed the menu, so this is the only correct
     * order: the layout is read after the answer, never in parallel. */
    if (!layout_send())
        fallback("could not ask for the layout");
}

/* An answer to a call this menu sent. Matched by serial, and only while
 * the epoch it was sent in is still the current one, so a late answer to a
 * menu that has been closed or replaced is dropped instead of applied. */
static void
reply_apply(DBusMessage *msg)
{
    /* The serial of the *call*, not of this message: a reply is a message
     * in its own right and carries its own serial, while libdbus keeps
     * the one it is answering in the reply serial. Matching on the wrong
     * one silently drops every answer. */
    dbus_uint32_t serial = dbus_message_get_reply_serial(msg);

    if (serial && serial == menu.serial_about) {
        bool stale = menu.gen != menu.gen_about;

        menu.serial_about = 0;
        if (!stale)
            about_apply(msg);
        return;
    }
    if (serial && serial == menu.serial_layout) {
        bool stale = menu.gen != menu.gen_layout;

        menu.serial_layout = 0;
        if (!stale)
            layout_apply(msg);
    }
}

/* ---- the seam, called from src/tray.c -------------------------------- */

bool
traymenu_open(const traymenu_req_t *req)
{
    if (!req || !trayint_bus())
        return false;
    if (!req->service || !*req->service || !req->path || !*req->path ||
        !req->menu_path || !*req->menu_path)
        return false;
    /* whatever was open is finished with, including its "closed" event */
    tray_menu_close();

    /* copied straight out: the request points into the item table, which
     * moves when an item is removed */
    snprintf(menu.service, sizeof(menu.service), "%s", req->service);
    snprintf(menu.path, sizeof(menu.path), "%s", req->path);
    snprintf(menu.menu_path, sizeof(menu.menu_path), "%s", req->menu_path);
    snprintf(menu.theme_path, sizeof(menu.theme_path), "%s",
        req->theme_path ? req->theme_path : "");
    menu.anchor_x = req->root_x;
    menu.anchor_y = req->root_y;
    rows_clear();
    memset(menu.title, 0, sizeof(menu.title));

    /* The menu object is the item's own path often enough to look like it
     * is always, which is why this registers it explicitly rather than
     * relying on the registration the item table already holds: for a
     * standard KDE item the menu is at /MenuBar and nothing else has
     * registered that path, so its signals would never be routed here. */
    generation_bump();
    trayint_path_add(menu.menu_path);
    if (!menu.matched) {
        menu.matched = true;
        trayint_add_match("type='signal',interface='" MENU_IFACE "'");
    }
    publish(TRAY_MENU_LOADING);
    menu.deadline = trayint_now_ms() + TRAYMENU_TIMEOUT_MS;

    /* the protocol wants the root event before the exchange, and neither
     * send can block: Event is fire and forget, AboutToShow is tracked */
    event_send(0, "opened");
    menu.opened = true;
    if (!about_send()) {
        menu.opened = false;
        fallback("could not ask the menu to show itself");
        return true;   /* the exchange was started and has failed */
    }
    return true;
}

bool
traymenu_dispatch(DBusMessage *msg)
{
    const char *iface, *member, *path;
    int type;

    if (!msg)
        return false;
    type = dbus_message_get_type(msg);
    if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN ||
        type == DBUS_MESSAGE_TYPE_ERROR) {
        reply_apply(msg);
        return true;
    }
    if (type != DBUS_MESSAGE_TYPE_SIGNAL || menu.state == TRAY_MENU_HIDDEN ||
        !menu.menu_path[0])
        return false;
    iface = dbus_message_get_interface(msg);
    member = dbus_message_get_member(msg);
    if (!iface || !member || strcmp(iface, MENU_IFACE))
        return false;
    /* only the menu object, never the item's own path: a standard KDE
     * item's signals come from /MenuBar, not from /StatusNotifierItem */
    path = dbus_message_get_path(msg);
    if (path && strcmp(path, menu.menu_path))
        return false;
    /* The sender is deliberately not checked. A signal carries the
     * emitting connection's unique name, while this seam only ever hands
     * over the well-known one, so comparing the two could never match and
     * every LayoutUpdated would be dropped. The path and the interface
     * are what identify the object, and the worst a stray signal can do
     * is provoke one coalesced round trip to the real service. */
    if (strcmp(member, "LayoutUpdated") &&
        strcmp(member, "ItemsPropertiesUpdated"))
        return false;
    /* Coalesced: one refetch however many signals arrive, and the rows stay
     * published until the new layout lands, so a signal storm redraws
     * once instead of flickering. Bump the epoch now, because a reader
     * that acts on the signal must not act on the rows it just saw. */
    menu.refetch = true;
    menu.epoch++;
    return true;
}

void
traymenu_round(void)
{
    if (menu.state == TRAY_MENU_HIDDEN)
        return;
    if (menu.state == TRAY_MENU_READY && menu.refetch) {
        menu.refetch = false;
        /* a failed refetch keeps the rows on screen: stale labels beat a
         * popup that goes blank, and the next LayoutUpdated tries again */
        (void)layout_send();
        return;
    }
    if (menu.state == TRAY_MENU_LOADING && menu.deadline &&
        trayint_now_ms() >= menu.deadline)
        fallback("no answer in time");
}

void
traymenu_item_gone(const char *service, const char *path)
{
    if (!menu.path[0] || !service || !path)
        return;
    if (strcmp(menu.service, service) || strcmp(menu.path, path))
        return;
    /* No Event("closed"): the client is gone, and the bus may already have
     * dropped it. Pending calls are only unreffed on this side, never
     * cancelled, because this can run inside a message handler where
     * libdbus documents cancelling as unsafe. */
    generation_bump();
    retire();
    rows_clear();
    identity_clear();
    publish(TRAY_MENU_HIDDEN);
}

/* ---- the public contract from src/tray.h ------------------------------ */

void
tray_menu_close(void)
{
    if (menu.state == TRAY_MENU_HIDDEN || !menu.path[0]) {
        retire();
        rows_clear();
        return;
    }
    if (menu.opened)
        event_send(0, "closed");
    generation_bump();
    retire();
    rows_clear();
    identity_clear();
    publish(TRAY_MENU_HIDDEN);
}

void
tray_menu_click(unsigned row)
{
    if (menu.state != TRAY_MENU_READY || row >= menu.nrows)
        return;
    if (!menu.rows[row].enabled ||
        menu.rows[row].type == TRAY_MENU_ROW_SEPARATOR)
        return;
    event_send((int32_t)menu.rows[row].id, "clicked");
    /* the protocol closes the root after a click, and the popup with it */
    tray_menu_close();
}

tray_menu_view_t
tray_menu_view(void)
{
    tray_menu_view_t v;

    memset(&v, 0, sizeof(v));
    v.state = menu.state;
    v.epoch = menu.epoch;
    v.anchor_x = menu.anchor_x;
    v.anchor_y = menu.anchor_y;
    v.title = menu.title;
    v.rows = menu.nrows ? menu.rows : NULL;
    v.nrows = menu.nrows;
    return v;
}

#else /* AUSTERE_NO_DBUS */

void
tray_menu_close(void)
{
}

void
tray_menu_click(unsigned row)
{
    (void)row;
}

tray_menu_view_t
tray_menu_view(void)
{
    tray_menu_view_t v;

    memset(&v, 0, sizeof(v));
    v.state = TRAY_MENU_HIDDEN;
    v.title = "";
    return v;
}

#endif /* AUSTERE_NO_DBUS */

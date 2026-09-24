#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notify.h"
#include "popup.h"
#include "settings.h"
#include "util.h"

#ifndef AUSTERE_NO_DBUS
#include <dbus/dbus.h>

#define NOTIFY_NAME "org.freedesktop.Notifications"
#define NOTIFY_PATH "/org/freedesktop/Notifications"
#define NOTIFY_IFACE "org.freedesktop.Notifications"

static DBusConnection *bus;
static bool name_owned;

static void
send_closed(unsigned id, unsigned reason)
{
    DBusMessage *sig = dbus_message_new_signal(NOTIFY_PATH,
        NOTIFY_IFACE, "NotificationClosed");
    uint32_t u_id = id, u_reason = reason;
    DBusMessageIter it;

    if (!sig)
        return;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &u_id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &u_reason);
    dbus_connection_send(bus, sig, NULL);
    dbus_message_unref(sig);
}

static void
send_action(unsigned id, const char *key)
{
    DBusMessage *sig = dbus_message_new_signal(NOTIFY_PATH,
        NOTIFY_IFACE, "ActionInvoked");
    uint32_t u_id = id;
    DBusMessageIter it;

    if (!sig || !key)
        return;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &u_id);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
    dbus_connection_send(bus, sig, NULL);
    dbus_message_unref(sig);
}

/* Toast stack -> D-Bus signals. */
static void
on_toast_closed(unsigned id, unsigned reason, void *ud)
{
    (void)ud;
    if (name_owned)
        send_closed(id, reason);
}

static void
on_toast_action(unsigned id, const char *key, void *ud)
{
    (void)ud;
    if (name_owned)
        send_action(id, key);
}

/* Hints austere acts on: urgency (byte), resident (bool) and the icon
 * path modern clients (notify-send, GTK/Qt) send instead of filling the
 * spec's app_icon field. Returns a malloc'd icon path, or NULL. */
static char *
read_hints(DBusMessageIter *dict, int *urgency, bool *resident)
{
    DBusMessageIter e;
    char *icon = NULL;

    /* recursing into a{sv} lands on the DICT_ENTRY, not the key */
    dbus_message_iter_recurse(dict, &e);
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, val;
        const char *key = NULL;

        dbus_message_iter_recurse(&e, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
            break;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);   /* now on the variant */
        if (key && dbus_message_iter_get_arg_type(&entry) ==
            DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &val);
            if (!strcmp(key, "urgency")) {
                uint8_t v = 0;

                if (dbus_message_iter_get_arg_type(&val) ==
                    DBUS_TYPE_BYTE)
                    dbus_message_iter_get_basic(&val, &v);
                *urgency = v;
            } else if (!strcmp(key, "resident")) {
                if (dbus_message_iter_get_arg_type(&val) ==
                    DBUS_TYPE_BOOLEAN)
                    dbus_message_iter_get_basic(&val, resident);
            } else if (!strcmp(key, "image-path") ||
                       !strcmp(key, "image_path")) {
                if (dbus_message_iter_get_arg_type(&val) ==
                    DBUS_TYPE_STRING) {
                    const char *p = NULL;

                    dbus_message_iter_get_basic(&val, &p);
                    free(icon);
                    icon = xstrdup(p ? p : "");
                }
            }
        }
        dbus_message_iter_next(&e);
    }
    return icon;
}

static void
method_notify(wm_t *wm, DBusMessage *msg)
{
    DBusMessageIter it, arr;
    /* libdbus hands out pointers into its own scratch that the next
     * read can invalidate, so every string is copied the moment it is
     * read rather than kept until the toast is built. */
    char *app = NULL, *icon = NULL, *summary = NULL, *body = NULL;
    uint32_t replaces = 0;
    int32_t timeout = -1;
    int urgency = 0;
    bool resident = false;
    toast_action_t actions[8];
    unsigned nactions = 0;

    dbus_message_iter_init(msg, &it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
        return;
    {
        const char *s = NULL;

        dbus_message_iter_get_basic(&it, &s);
        app = xstrdup(s ? s : "");
    }
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &replaces);
    dbus_message_iter_next(&it);
    {
        const char *s = NULL;

        dbus_message_iter_get_basic(&it, &s);
        icon = xstrdup(s ? s : "");
    }
    dbus_message_iter_next(&it);
    {
        const char *s = NULL;

        dbus_message_iter_get_basic(&it, &s);
        summary = xstrdup(s ? s : "");
    }
    dbus_message_iter_next(&it);
    {
        const char *s = NULL;

        dbus_message_iter_get_basic(&it, &s);
        body = xstrdup(s ? s : "");
    }
    dbus_message_iter_next(&it);

    /* actions: a FLAT array of alternating key/label strings (the spec
     * type is "as"), so a trailing odd key is ignored */
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        char *pair[2] = { NULL, NULL };
        unsigned slot = 0;

        dbus_message_iter_recurse(&it, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            const char *s = NULL;

            dbus_message_iter_get_basic(&arr, &s);
            free(pair[slot]);
            pair[slot] = xstrdup(s ? s : "");
            if (slot == 1) {
                if (nactions < 8) {
                    actions[nactions].key = pair[0];
                    actions[nactions].label = pair[1][0]
                        ? pair[1] : xstrdup(pair[0]);
                    nactions++;
                    pair[0] = pair[1] = NULL;
                } else {
                    free(pair[0]);
                    pair[0] = NULL;
                }
            }
            slot ^= 1;
            dbus_message_iter_next(&arr);
        }
        free(pair[0]);
        free(pair[1]);
    }
    dbus_message_iter_next(&it); /* past actions */

    char *hint_icon = NULL;

    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY)
        hint_icon = read_hints(&it, &urgency, &resident);
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32)
        dbus_message_iter_get_basic(&it, &timeout);

    /* app_icon is the spec's field; image-path is what clients actually
     * send, so it wins when the former is empty */
    const char *icon_arg = (icon && *icon) ? icon
        : (hint_icon && *hint_icon) ? hint_icon : NULL;

    unsigned id = toast_add(wm, app, summary, body, icon_arg, urgency,
        (int)timeout, replaces, resident, actions, nactions);
    free(hint_icon);
    free(app);
    free(icon);
    free(summary);
    free(body);
    for (unsigned i = 0; i < nactions; i++) {
        free(actions[i].key);
        free(actions[i].label);
    }
    DBusMessage *reply = dbus_message_new_method_return(msg);
    uint32_t u_id = id;
    DBusMessageIter ri;

    if (reply) {
        dbus_message_iter_init_append(reply, &ri);
        dbus_message_iter_append_basic(&ri, DBUS_TYPE_UINT32, &u_id);
        dbus_connection_send(bus, reply, NULL);
        dbus_message_unref(reply);
    }
}

static DBusHandlerResult
on_message(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    wm_t *wm = user_data;

    if (dbus_message_is_method_call(msg, NOTIFY_IFACE, "Notify")) {
        method_notify(wm, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE,
            "CloseNotification")) {
        uint32_t id = 0;
        DBusMessageIter it;

        if (dbus_message_iter_init(msg, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&it, &id);
            toast_close(wm, id, TOAST_CLOSED_PROGRAMMATIC);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE,
            "GetCapabilities")) {
        static const char *caps[] = {
            "body", "body-markup", "icon-static", "actions", NULL
        };
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter it, arr;

        if (reply) {
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
            for (unsigned i = 0; caps[i]; i++)
                dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING,
                    &caps[i]);
            dbus_message_iter_close_container(&it, &arr);
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE,
            "GetServerInformation")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        const char *name = "austere";
        const char *vendor = "austere";
        const char *version = "0.1";
        const char *spec = "1.2";
        DBusMessageIter it;

        if (reply) {
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &vendor);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &version);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &spec);
            dbus_connection_send(conn, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
#endif /* !AUSTERE_NO_DBUS */

void
notify_init(wm_t *wm)
{
    (void)wm;   /* unused in an AUSTERE_NO_DBUS build */
#ifndef AUSTERE_NO_DBUS
    DBusError err;

    if (!cfg.notify_enabled) {
        fprintf(stderr, "austere: notifications: disabled in config\n");
        return;
    }
    dbus_error_init(&err);
    bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!bus) {
        fprintf(stderr, "austere: notifications: no session bus: %s\n",
            err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);

    static DBusObjectPathVTable vt; /* zero-init: only the handler */

    vt.message_function = on_message;
    if (!dbus_connection_register_object_path(bus, NOTIFY_PATH, &vt, wm)) {
        fprintf(stderr, "austere: notifications: cannot export %s\n",
            NOTIFY_PATH);
        return;
    }

    int rc = dbus_bus_request_name(bus, NOTIFY_NAME,
        DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);

    name_owned = rc == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        rc == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
    if (name_owned)
        fprintf(stderr, "austere: notifications: serving %s\n", NOTIFY_NAME);
    else
        fprintf(stderr, "austere: notifications: %s is taken by another"
            " daemon%s%s - stop it to let austere serve notifications\n",
            NOTIFY_NAME, err.message ? ": " : "",
            err.message ? err.message : "");
    dbus_error_free(&err);
    popups_set_closed_cb(on_toast_closed, NULL);
    popups_set_action_cb(on_toast_action, NULL);
#endif
}

void
notify_shutdown(wm_t *wm)
{
    (void)wm;
#ifndef AUSTERE_NO_DBUS
    if (bus) {
        /* closing the connection releases the name */
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        bus = NULL;
    }
    name_owned = false;
    popups_set_closed_cb(NULL, NULL);
    popups_set_action_cb(NULL, NULL);
#endif
}

int
notify_fd(void)
{
#ifndef AUSTERE_NO_DBUS
    if (!bus || !name_owned)
        return -1;
    int fd = -1;

    if (!dbus_connection_get_unix_fd(bus, &fd))
        return -1;
    return fd;
#else
    return -1;
#endif
}

void
notify_pump(wm_t *wm)
{
    (void)wm;
#ifndef AUSTERE_NO_DBUS
    if (!bus || !name_owned)
        return;
    /* One non-blocking read pass, then convert whatever it produced.
     * dbus_connection_read_write_dispatch() is wrong here: it keeps
     * returning true while the connection has queued traffic (a reply
     * we just queued counts), so looping on it spins forever and
     * wedges the WM's event loop. */
    dbus_connection_read_write(bus, 0);
    while (dbus_connection_get_dispatch_status(bus) !=
           DBUS_DISPATCH_COMPLETE) {
        if (dbus_connection_dispatch(bus) != DBUS_DISPATCH_DATA_REMAINS)
            break;
    }
#endif
}

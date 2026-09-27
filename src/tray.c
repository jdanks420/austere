#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tray.h"
#include "icons.h"

/* The visible snapshot, backend-owned and stable. */
static tray_item_t view[TRAY_MAX_ITEMS];
static unsigned nview;

#ifndef AUSTERE_NO_DBUS

#include <dbus/dbus.h>
#include <unistd.h>

#include "tray_int.h"

/* Maps every snapshot index back to its table slot, so an action can be
 * resolved to a stable identity (copied out) before any bus traffic. */
static unsigned view_src[TRAY_MAX_ITEMS];

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"
#define PEER_IFACE "org.freedesktop.DBus.Peer"
#define DBUS_IFACE_FULL "org.freedesktop.DBus"

/* Well-known name the host claims, as the spec's examples do. */
#define HOST_NAME_PREFIX "org.kde.StatusNotifierHost-"
#define ITEM_PATH_DEFAULT "/StatusNotifierItem"

#define TRAY_SERVICE_MAX 160
#define TRAY_PATH_MAX 192
#define TRAY_ICON_MAX 160
#define TRAY_MENU_MAX 192
/* IconThemePath is a remote string used as a path prefix, so it gets the
 * same treatment as a name: bounded, and checked again per root inside
 * icon_resolve_ex(). */
#define TRAY_THEME_PATH_MAX 512
#define TRAY_ID_MAX (TRAY_SERVICE_MAX + TRAY_PATH_MAX + 2)
#define TRAY_RULE_MAX (TRAY_SERVICE_MAX + TRAY_PATH_MAX + 96)

/* A dead item lingers this long so an app restart does not flicker. */
#define TRAY_DEATH_GRACE_MS 500
/* A property fetch that is never answered is re-armed after this long. */
#define TRAY_FETCH_TIMEOUT_MS 1500
/* How long tray_timeout_ms() keeps the loop awake for a menu exchange.
 * The client's own deadline is shorter, so the client is always the one
 * that decides; this only guarantees a round happens after that. It is
 * a one-shot wakeup: once it passes the tray is idle again, so a menu
 * that was dismissed, answered or failed costs nothing afterwards. */
#define TRAY_MENU_HINT_MS 2000
/* Backoff for a GetAll the pump could not send, so tray_timeout_ms wakes
 * the loop to retry it instead of stalling until unrelated bus traffic. */
#define TRAY_REFRESH_RETRY_MS 250
#define TRAY_DEFAULT_ICON_PX 16
/* Hostile clients cannot make us allocate an unbounded pixmap. */
#define TRAY_PIXMAP_MAX_PX 512

typedef struct {
    char service[TRAY_SERVICE_MAX]; /* canonical, as registered */
    char path[TRAY_PATH_MAX];
    char id[TRAY_ID_MAX];           /* service + path, the wire ID */
    char owner[TRAY_SERVICE_MAX];   /* sender's unique name at registration */
    char title[TRAY_TITLE_MAX];
    char icon_name[TRAY_ICON_MAX];      /* IconName, raw */
    char attn_name[TRAY_ICON_MAX];      /* AttentionIconName, raw */
    char menu[TRAY_MENU_MAX];
    char theme_path[TRAY_THEME_PATH_MAX];  /* IconThemePath, "" if none */
    tray_status_t status;
    bool is_menu;
    bool dead;                    /* owner vanished: inside the grace */
    bool icon_warned;             /* already logged the no-icon case */
    long long grace_until;        /* monotonic ms */
    image_t *img;                 /* active image, owned by this item */
    uint32_t *px;                 /* its pixels when we own them */
    uint32_t *fbpx;               /* IconPixmap fallback pixels */
    unsigned fbw, fbh;
    dbus_uint32_t pending;        /* in-flight GetAll serial, 0 = none */
    long long pending_since;
    unsigned gen;                 /* bumped by every item signal */
    unsigned gen_sent;            /* gen when the GetAll went out */
    bool needs_refresh;
} ti_t;

/* Fixed table: the table itself never moves items, only the view does. */
static ti_t items[TRAY_MAX_ITEMS];
static unsigned nitems;

static DBusConnection *bus;
static bool watcher_owned;      /* we are the org.kde.StatusNotifierWatcher */
static bool name_retry;         /* watcher name freed: ask again */
static char host_name[96];
static bool host_sig_sent;

/* libdbus routes an incoming message to the vtable registered for the
 * path it carries and drops anything else, so item signals need their
 * object path registered here too, and so do the daemon's own signals
 * (NameOwnerChanged carries /org/freedesktop/DBus). Paths are reference
 * counted because many items share /StatusNotifierItem. */
typedef struct {
    char path[TRAY_PATH_MAX];
    unsigned refs;
} reg_t;

#define REG_MAX (TRAY_MAX_ITEMS + 1)
static reg_t regs[REG_MAX];
static unsigned nregs;
static DBusObjectPathVTable vtable;   /* zero-init: only the handler */

static void
paths_clear(void)
{
    for (unsigned i = 0; i < nregs; i++)
        dbus_connection_unregister_object_path(bus, regs[i].path);
    nregs = 0;
}

static void
path_add(const char *path)
{
    for (unsigned i = 0; i < nregs; i++) {
        if (strcmp(regs[i].path, path))
            continue;
        regs[i].refs++;
        return;
    }
    if (nregs == REG_MAX || !dbus_connection_register_object_path(bus, path,
            &vtable, NULL)) {
        /* signals from this item are lost, the registry entry is not */
        return;
    }
    snprintf(regs[nregs].path, sizeof(regs[nregs].path), "%s", path);
    regs[nregs].refs = 1;
    nregs++;
}

static void
path_drop(const char *path)
{
    for (unsigned i = 0; i < nregs; i++) {
        if (strcmp(regs[i].path, path))
            continue;
        if (--regs[i].refs)
            return;
        dbus_connection_unregister_object_path(bus, regs[i].path);
        regs[i] = regs[nregs - 1];
        nregs--;
        return;
    }
}

static unsigned icon_px = TRAY_DEFAULT_ICON_PX;
static unsigned icon_px_set;      /* tray_set_icon_size() already applied */
static bool view_dirty;
static bool render_pending;       /* the snapshot changed; UI must repaint */
static uint64_t view_sig;
static bool full_logged;          /* the full-table warning is one-shot */
static long long menu_hint_until; /* wake the loop for the menu's deadline */

/* A closed or dropped bus connection must stop being polled: its
 * descriptor stays readable and would spin the event loop. */
static bool
bus_live(void)
{
    return bus && dbus_connection_get_is_connected(bus);
}

static long long
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
hash_bytes(uint64_t *h, const void *p, size_t n)
{
    const unsigned char *b = p;

    for (size_t i = 0; i < n; i++)
        *h = (*h ^ b[i]) * 1099511628211ULL;
}

static void
hash_str(uint64_t *h, const char *s)
{
    if (s)
        hash_bytes(h, s, strlen(s) + 1);
}

/* ---- images --------------------------------------------------------- */

/* An untrusted icon name must not be able to walk out of the icon
 * directories: icons.c takes a leading '/' as a literal path. */
static bool
icon_name_ok(const char *s)
{
    if (!s || !*s || strlen(s) >= TRAY_ICON_MAX)
        return false;
    return strstr(s, "..") == NULL;
}

/* IconThemePath is a colon-separated list of directories the item wants
 * searched first. It is remote and it becomes a path prefix, so it is
 * held to the same rule as the roots icon_resolve_ex() accepts: absolute
 * entries, no "..", nothing that would overflow a path buffer. A value
 * that fails is dropped rather than stored, so a later good value can
 * take its place. */
static bool
theme_path_ok(const char *s)
{
    size_t n = strlen(s);

    if (!n || n >= TRAY_THEME_PATH_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f)
            return false;
        if (c == '.' && i + 1 < n && s[i + 1] == '.')
            return false;
    }
    /* at least one usable absolute root, or there is nothing to keep */
    return s[0] == '/' || strchr(s, ':') != NULL;
}

static void
img_clear(ti_t *it)
{
    free(it->img);
    free(it->px);
    it->img = NULL;
    it->px = NULL;
}

static void
img_set(ti_t *it, uint32_t *px, unsigned w, unsigned h)
{
    if (!px || !w || !h)
        return;
    it->img = calloc(1, sizeof(*it->img));
    if (!it->img) {
        free(px);
        return;
    }
    it->img->argb = px;
    it->img->w = w;
    it->img->h = h;
    it->px = px;
}

/* Themed names first (Attention wins for NeedsAttention), then the
 * IconPixmap fallback the GetAll pass left us. An item whose icon
 * resolves to nothing stays visible with img == NULL. */
static void
icon_pick(ti_t *it)
{
    const char *cand[2];
    unsigned ncand = 0;
    const char *picked = NULL;
    const char *theme = it->theme_path[0] ? it->theme_path : NULL;

    /* the old image goes first: a re-resolve must not leak it, and the
     * pixmap fallback below reuses fbpx instead of owning a copy */
    img_clear(it);
    if (it->status == TRAY_STATUS_ATTENTION && it->attn_name[0])
        cand[ncand++] = it->attn_name;
    if (it->icon_name[0])
        cand[ncand++] = it->icon_name;
    for (unsigned i = 0; i < ncand; i++) {
        const image_t *src;
        uint32_t *copy;
        size_t n;

        if (!icon_name_ok(cand[i]))
            continue;
        /* icon_resolve_ex, never icon_get: a client picks this name, so
         * nothing about it may enter the process-lifetime cache. The
         * result is the resolver's and is valid only until the next
         * call, which is why it is copied out immediately and never
         * freed here. Its own IconThemePath is passed so a themed
         * indicator resolves the artwork it means. */
        src = icon_resolve_ex(cand[i], icon_px, theme);
        if (!src || !src->argb || !src->w || !src->h)
            continue;
        n = (size_t)src->w * src->h * sizeof(uint32_t);
        copy = malloc(n);
        if (!copy)
            break;   /* the resolver reclaims its own image for us */
        memcpy(copy, src->argb, n);
        img_set(it, copy, src->w, src->h);
        picked = cand[i];
        it->icon_warned = false;
        break;
    }
    if (picked || it->icon_warned)
        return;
    if (it->fbpx && it->fbw && it->fbh) {
        it->img = calloc(1, sizeof(*it->img));
        if (it->img) {
            it->img->argb = it->fbpx;
            it->img->w = it->fbw;
            it->img->h = it->fbh;
        }
        /* only the fallback cases are worth a line, and only once per
         * episode: a themed name that worked needs no noise, but a
         * pixmap-only or icon-less item is what an interop complaint is
         * about */
        it->icon_warned = true;
        fprintf(stderr, "austere: tray: %s: no icon name resolved,"
            " using IconPixmap %ux%u\n", it->id, it->fbw, it->fbh);
    } else if (ncand) {
        it->icon_warned = true;
        fprintf(stderr, "austere: tray: %s: no icon for \"%s\"%s%s%s\n",
            it->id, it->status == TRAY_STATUS_ATTENTION ? it->attn_name
            : it->icon_name,
            it->status == TRAY_STATUS_ATTENTION && it->icon_name[0] ?
            " / \"" : "", it->status == TRAY_STATUS_ATTENTION &&
            it->icon_name[0] ? it->icon_name : "",
            it->status == TRAY_STATUS_ATTENTION && it->icon_name[0] ?
            "\"" : "");
    }
}

/* IconPixmap is a(iiay): pick the square closest to the target (a
 * non-square only when nothing square is offered) and convert it from
 * network byte order into the project's host-order ARGB. */
static void
pixmap_parse(ti_t *it, DBusMessageIter *arr)
{
    DBusMessageIter e;
    const uint8_t *best = NULL;
    int32_t bw = 0, bh = 0;
    int best_score = INT_MAX;
    uint32_t *px;

    free(it->fbpx);
    it->fbpx = NULL;
    it->fbw = it->fbh = 0;
    dbus_message_iter_recurse(arr, &e);
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRUCT) {
        DBusMessageIter st;
        const uint8_t *bytes = NULL;
        int nbytes = 0, score;
        int32_t w = 0, h = 0;

        dbus_message_iter_recurse(&e, &st);
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
            goto next;
        dbus_message_iter_get_basic(&st, &w);
        dbus_message_iter_next(&st);
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
            goto next;
        dbus_message_iter_get_basic(&st, &h);
        dbus_message_iter_next(&st);
        if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY &&
            dbus_message_iter_get_element_type(&st) == DBUS_TYPE_BYTE) {
            DBusMessageIter data;

            dbus_message_iter_recurse(&st, &data);
            dbus_message_iter_get_fixed_array(&data, (void **)&bytes,
                &nbytes);
        }
        if (w > 0 && h > 0 && w <= TRAY_PIXMAP_MAX_PX &&
            h <= TRAY_PIXMAP_MAX_PX && bytes &&
            (long long)w * h * 4 <= nbytes) {
            int d = (int)(w > h ? w : h) - (int)icon_px;

            if (d < 0)
                d = -d;
            score = (w == h) ? d : d + 1000;
            if (score < best_score) {
                best_score = score;
                best = bytes;
                bw = w;
                bh = h;
            }
        }
next:
        dbus_message_iter_next(&e);
    }
    if (!best)
        return;
    px = malloc((size_t)bw * (size_t)bh * sizeof(uint32_t));
    if (!px)
        return;
    for (int32_t i = 0, n = bw * bh; i < n; i++) {
        const uint8_t *b = best + (size_t)i * 4;

        px[i] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
            ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    }
    it->fbpx = px;
    it->fbw = (unsigned)bw;
    it->fbh = (unsigned)bh;
}

/* ---- item table ----------------------------------------------------- */

static ti_t *
item_find(const char *id)
{
    for (unsigned i = 0; i < nitems; i++)
        if (!strcmp(items[i].id, id))
            return &items[i];
    return NULL;
}

static ti_t *
item_by_signal(DBusMessage *msg)
{
    const char *path = dbus_message_get_path(msg);
    const char *sender = dbus_message_get_sender(msg);

    if (!path)
        return NULL;
    for (unsigned i = 0; i < nitems; i++) {
        ti_t *it = &items[i];

        if (strcmp(it->path, path))
            continue;
        if (sender && *sender && strcmp(sender, it->owner) &&
            strcmp(sender, it->service))
            continue;
        return it;
    }
    return NULL;
}

/* Defined with the pending table below; item_reset() has to retire an
 * item's in-flight GetAll before the slot is wiped. `may_cancel` is false
 * inside a message handler, where libdbus documents
 * dbus_pending_call_cancel() as unsafe because it locks the connection
 * again; there the call is only unreffed, and libdbus consumes and drops
 * a late reply by itself. */
static void pend_drop_serial(dbus_uint32_t serial, bool may_cancel);

static void
item_reset(ti_t *it, bool from_handler)
{
    /* Retire the in-flight fetch first: the memset would otherwise leave
     * a live DBusPendingCall in pends[] that no item can ever match
     * (pend_harvest looks the item up by serial), leaking one table slot
     * per reset until PEND_MAX is exhausted and every later GetAll,
     * including the watcher-name re-request, is refused. */
    if (it->pending) {
        pend_drop_serial(it->pending, !from_handler);
        it->pending = 0;
    }
    /* the menu client keeps its own copy of the identity, so it can be
     * told before the table loses it; the memset below would leave it
     * talking to a slot that no longer means anything */
    traymenu_item_gone(it->service, it->path);
    img_clear(it);
    free(it->fbpx);
    memset(it, 0, sizeof(*it));
}

enum { ITEM_FAILED, ITEM_NEW, ITEM_RESET };

/* Add, or reset an existing entry: a client that re-registers after
 * re-exporting its object is a new item, never a duplicate. */
static int
item_add(const char *service, const char *path, const char *owner,
    ti_t **out)
{
    char id[TRAY_ID_MAX];
    ti_t *it = NULL;
    int kind = ITEM_NEW;

    snprintf(id, sizeof(id), "%s%s", service, path);
    it = item_find(id);
    if (it) {
        item_reset(it, true);
        kind = ITEM_RESET;
    } else {
        if (nitems == TRAY_MAX_ITEMS) {
            if (!full_logged) {
                full_logged = true;
                fprintf(stderr, "austere: tray: registry full (%u items),"
                    " ignoring %s\n", (unsigned)TRAY_MAX_ITEMS, id);
            }
            return ITEM_FAILED;
        }
        it = &items[nitems++];
        memset(it, 0, sizeof(*it));
    }
    snprintf(it->service, sizeof(it->service), "%s", service);
    snprintf(it->path, sizeof(it->path), "%s", path);
    snprintf(it->id, sizeof(it->id), "%s", id);
    snprintf(it->owner, sizeof(it->owner), "%s", owner ? owner : service);
    /* the spec's default is Active; the first GetAll refines it */
    it->status = TRAY_STATUS_ACTIVE;
    it->needs_refresh = true;
    view_dirty = true;
    *out = it;
    return kind;
}

static void
item_remove(unsigned idx)
{
    char buf[TRAY_ID_MAX];
    const char *id = buf;   /* append_basic wants a char *, not an array */
    ti_t *it = &items[idx];

    snprintf(buf, sizeof(buf), "%s", it->id);
    path_drop(it->path);
    item_reset(it, false);
    if (idx + 1 < nitems)
        memmove(&items[idx], &items[idx + 1],
            (nitems - idx - 1) * sizeof(items[0]));
    nitems--;
    view_dirty = true;
    if (watcher_owned) {
        DBusMessage *sig = dbus_message_new_signal(WATCHER_PATH,
            WATCHER_IFACE, "StatusNotifierItemUnregistered");
        DBusMessageIter iter;

        if (sig) {
            dbus_message_iter_init_append(sig, &iter);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &id);
            dbus_connection_send(bus, sig, NULL);
            dbus_message_unref(sig);
        }
    }
    fprintf(stderr, "austere: tray: %s unregistered\n", id);
}

static void
item_mark_dead(ti_t *it)
{
    if (it->dead)
        return;
    it->dead = true;
    it->grace_until = now_ms() + TRAY_DEATH_GRACE_MS;
    view_dirty = true;
}

/* ---- the visible snapshot ------------------------------------------- */

static void
view_rebuild(void)
{
    uint64_t h = 1469598103934665603ULL;
    unsigned n = 0;

    for (unsigned i = 0; i < nitems; i++) {
        ti_t *it = &items[i];
        tray_item_t *v;

        if (it->status == TRAY_STATUS_PASSIVE)
            continue;
        v = &view[n];
        view_src[n] = i;
        snprintf(v->title, sizeof(v->title), "%s", it->title);
        v->status = it->status;
        v->icon_name = "";
        if (it->status == TRAY_STATUS_ATTENTION && it->attn_name[0])
            v->icon_name = it->attn_name;
        else if (it->icon_name[0])
            v->icon_name = it->icon_name;
        v->img = it->img;
        v->menu_path = it->menu[0] ? it->menu : "";
        /* not hashed below: ItemIsMenu changes what a press does, not
         * what the bar draws, so a change in it must not cost a repaint.
         * The button path reads the live view, never a copy of it. */
        v->is_menu = it->is_menu;
        n++;
    }
    hash_bytes(&h, &n, sizeof(n));
    for (unsigned i = 0; i < n; i++) {
        hash_str(&h, view[i].title);
        hash_bytes(&h, &view[i].status, sizeof(view[i].status));
        hash_str(&h, view[i].icon_name);
        hash_str(&h, view[i].menu_path);
        hash_bytes(&h, &view[i].img, sizeof(view[i].img));
    }
    nview = n;
    /* a repaint is only needed when the snapshot really changed */
    if (h != view_sig) {
        view_sig = h;
        render_pending = true;
    }
}

static void
view_sync(void)
{
    if (view_dirty) {
        view_dirty = false;
        view_rebuild();
    }
}

/* ---- outgoing traffic ----------------------------------------------- */

/* Reply tracking. dbus_connection_send_with_reply() registers a pending
 * call without blocking; the reply is collected later in the pump with
 * dbus_pending_call_get_completed() + dbus_pending_call_steal_reply(),
 * both non-blocking. *_with_reply_and_block is never used: the WM thread
 * must not wait on the bus. libdbus also drops replies whose path has no
 * registered object, so a pending call is the only way to see a reply at
 * all; untracked calls (AddMatch) get their reply discarded, which is
 * exactly what we want. */
/* src/tray_int.h owns the canonical list, so the menu client and this
 * file cannot drift apart on what a pending call means. These aliases
 * keep the rest of the file as it was written; the menu kinds are used
 * by src/tray_menu.c through trayint_send_tracked(). */
#define PEND_NAME  TRAYINT_PEND_NAME
#define PEND_HOST  TRAYINT_PEND_HOST
#define PEND_PROPS TRAYINT_PEND_PROPS

/* Sized from the canonical kind count, not from a copy of it. */
#define PEND_MAX (TRAY_MAX_ITEMS + TRAYINT_PEND_KINDS)

typedef struct {
    DBusPendingCall *pc;
    dbus_uint32_t serial;
    long long since;    /* monotonic ms, so a menu call cannot hold a slot */
    int kind;
} pend_t;

static pend_t pends[PEND_MAX];
static unsigned npends;

static void
send_msg(DBusMessage *msg)
{
    if (!msg)
        return;
    dbus_connection_send(bus, msg, NULL);
    dbus_message_unref(msg);
}

static bool
send_tracked(DBusMessage *msg, int kind, dbus_uint32_t *serial_out)
{
    DBusPendingCall *pc = NULL;
    dbus_uint32_t serial;

    if (!msg)
        return false;
    if (npends == PEND_MAX) {
        dbus_message_unref(msg);
        return false;
    }
    if (!dbus_connection_send_with_reply(bus, msg, &pc,
            DBUS_TIMEOUT_USE_DEFAULT) || !pc) {
        dbus_message_unref(msg);
        return false;
    }
    serial = dbus_message_get_serial(msg);
    dbus_message_unref(msg);
    pends[npends].pc = pc;
    pends[npends].serial = serial;
    pends[npends].since = now_ms();
    pends[npends].kind = kind;
    npends++;
    if (serial_out)
        *serial_out = serial;
    return true;
}

static void
pend_drop_kind(int kind)
{
    for (unsigned i = 0; i < npends; i++) {
        if (pends[i].kind != kind)
            continue;
        dbus_pending_call_cancel(pends[i].pc);
        dbus_pending_call_unref(pends[i].pc);
        pends[i] = pends[npends - 1];
        npends--;
        return;
    }
}

static void
pend_drop_serial(dbus_uint32_t serial, bool may_cancel)
{
    for (unsigned i = 0; i < npends; i++) {
        if (pends[i].serial != serial)
            continue;
        if (may_cancel)
            dbus_pending_call_cancel(pends[i].pc);
        dbus_pending_call_unref(pends[i].pc);
        pends[i] = pends[npends - 1];
        npends--;
        return;
    }
}

static void
pend_clear(void)
{
    while (npends) {
        DBusPendingCall *pc;

        npends--;
        pc = pends[npends].pc;
        dbus_pending_call_cancel(pc);
        dbus_pending_call_unref(pc);
    }
}

/* AddMatch is sent fire-and-forget on purpose: it keeps the daemon's
 * answer out of the reply-tracking table (nothing useful to do with it)
 * and keeps the call off any blocking helper. The rule is in effect as
 * soon as the daemon has read it, which is ordered ahead of anything the
 * client sends us afterwards. */
static void
bus_add_match(const char *rule)
{
    DBusMessage *m = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
    DBusMessageIter it;

    if (!m)
        return;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &rule);
    send_msg(m);
}

/* ---- the internal seam (src/tray_int.h) ------------------------------
 *
 * One connection, one reply table, one set of registered object paths,
 * shared with the menu client in src/tray_menu.c. Every one of these is
 * a non-blocking pass-through to something above: the menu never opens a
 * second connection, never runs a second dispatch loop, and never
 * blocks. trayint_bus() answers NULL for a connection that is gone, so
 * the menu can fail open instead of sending into a dead socket.
 */
DBusConnection *
trayint_bus(void)
{
    return bus_live() ? bus : NULL;
}

long long
trayint_now_ms(void)
{
    return now_ms();
}

void
trayint_send(DBusMessage *msg)
{
    send_msg(msg);
}

bool
trayint_send_tracked(DBusMessage *msg, int kind, dbus_uint32_t *serial_out)
{
    return send_tracked(msg, kind, serial_out);
}

void
trayint_path_add(const char *path)
{
    path_add(path);
}

void
trayint_path_drop(const char *path)
{
    path_drop(path);
}

void
trayint_add_match(const char *rule)
{
    bus_add_match(rule);
}

static void
request_name(const char *name, int kind)
{
    DBusMessage *m = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "RequestName");
    DBusMessageIter it;
    dbus_uint32_t flags = DBUS_NAME_FLAG_DO_NOT_QUEUE;

    if (!m)
        return;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &flags);
    pend_drop_kind(kind);
    send_tracked(m, kind, NULL);
}

static void
item_call_xy(const char *service, const char *path, const char *member,
    int32_t x, int32_t y)
{
    DBusMessage *m = dbus_message_new_method_call(service, path, ITEM_IFACE,
        member);
    DBusMessageIter it;

    if (!m)
        return;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &x);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &y);
    send_msg(m);
}

static void
item_call_scroll(const char *service, const char *path, int32_t delta,
    bool horizontal)
{
    const char *orient = horizontal ? "horizontal" : "vertical";
    DBusMessage *m = dbus_message_new_method_call(service, path, ITEM_IFACE,
        "Scroll");
    DBusMessageIter it;

    if (!m)
        return;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &delta);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &orient);
    send_msg(m);
}

/* One GetAll in flight per item, matched by its serial. Coalesced: a
 * signal that lands mid-flight leaves needs_refresh set for the next
 * round instead of stacking another call. */
static void
props_send(ti_t *it)
{
    DBusMessage *m = dbus_message_new_method_call(it->service, it->path,
        PROPS_IFACE, "GetAll");
    DBusMessageIter mi;
    const char *iface = ITEM_IFACE;

    if (!m)
        return;
    dbus_message_iter_init_append(m, &mi);
    dbus_message_iter_append_basic(&mi, DBUS_TYPE_STRING, &iface);
    it->needs_refresh = false;
    it->gen_sent = it->gen;
    it->pending_since = now_ms();
    if (!send_tracked(m, PEND_PROPS, &it->pending))
        it->needs_refresh = true;   /* retried from the next pump */
}

static void
props_flush(void)
{
    for (unsigned i = 0; i < nitems; i++)
        if (items[i].needs_refresh && !items[i].pending)
            props_send(&items[i]);
}

/* ---- watcher object ------------------------------------------------- */

static void
reply_empty(DBusMessage *msg)
{
    send_msg(dbus_message_new_method_return(msg));
}

static void
reply_error(DBusMessage *msg, const char *name)
{
    send_msg(dbus_message_new_error(msg, name,
        "not supported by austere's tray watcher"));
}

static void
reply_error_msg(DBusMessage *msg, const char *name, const char *text)
{
    send_msg(dbus_message_new_error(msg, name, text));
}

/* The registry a client sees, as canonical "service+path" IDs.
 *
 * Interop note: the ID is the concatenation the spec describes, built
 * from the service the client registered (its unique name for the
 * bare-path form) plus the object path. Keeping the name the client
 * registered with - rather than whatever a well-known name resolves to
 * later - is what makes NameOwnerChanged tracking and re-registration
 * unambiguous, so one client always maps to one ID.
 *
 * Items inside the death grace are excluded: clients read this list as
 * the live registry, and one that saw its own ID vanish 500 ms before
 * the entry actually goes would re-register in a loop. */
static void
append_items(DBusMessageIter *arr)
{
    for (unsigned i = 0; i < nitems; i++) {
        const char *id;

        if (items[i].dead)
            continue;
        id = items[i].id;
        dbus_message_iter_append_basic(arr, DBUS_TYPE_STRING, &id);
    }
}

static void
emit_items_changed(void)
{
    static const char *key = "RegisteredStatusNotifierItems";
    const char *iface = WATCHER_IFACE;
    DBusMessage *m = dbus_message_new_signal(WATCHER_PATH, PROPS_IFACE,
        "PropertiesChanged");
    DBusMessageIter it, arr, entry, var, list, empty;

    if (!m)
        return;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL,
        &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    /* the variant holds an array, so both levels need their container */
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &list);
    append_items(&list);
    dbus_message_iter_close_container(&var, &list);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&it, &arr);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &empty);
    dbus_message_iter_close_container(&it, &empty);
    send_msg(m);
}

static bool
prop_known(const char *name)
{
    return name && (!strcmp(name, "IsStatusNotifierHostRegistered") ||
        !strcmp(name, "RegisteredStatusNotifierItems") ||
        !strcmp(name, "ProtocolVersion"));
}

static const char introspect_xml[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
    "Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"" INTROSPECT_IFACE "\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"" PEER_IFACE "\">\n"
    "    <method name=\"Ping\"/>\n"
    "    <method name=\"GetMachineId\">\n"
    "      <arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"" PROPS_IFACE "\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"PropertiesChanged\">\n"
    "      <arg name=\"interface_name\" type=\"s\"/>\n"
    "      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"" WATCHER_IFACE "\">\n"
    "    <method name=\"RegisterStatusNotifierItem\">\n"
    "      <arg name=\"service\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"RegisterStatusNotifierHost\">\n"
    "      <arg name=\"service\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <property name=\"IsStatusNotifierHostRegistered\" type=\"b\""
    " access=\"read\"/>\n"
    "    <property name=\"RegisteredStatusNotifierItems\" type=\"as\""
    " access=\"read\"/>\n"
    "    <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
    "    <signal name=\"StatusNotifierItemRegistered\">\n"
    "      <arg name=\"service\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"StatusNotifierItemUnregistered\">\n"
    "      <arg name=\"service\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"StatusNotifierHostRegistered\"/>\n"
    "    <signal name=\"StatusNotifierHostUnregistered\"/>\n"
    "  </interface>\n"
    "</node>\n";

static void
method_introspect(DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter it;
    const char *xml = introspect_xml;

    if (!reply)
        return;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &xml);
    send_msg(reply);
}

/* Read the n leading string arguments into fixed buffers. */
static bool
read_str_args(DBusMessage *msg, char *a, size_t an, char *b, size_t bn)
{
    DBusMessageIter it;
    const char *s = NULL;

    if (!dbus_message_iter_init(msg, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
        return false;
    dbus_message_iter_get_basic(&it, &s);
    snprintf(a, an, "%s", s ? s : "");
    if (!b)
        return true;
    if (!dbus_message_iter_next(&it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
        return false;
    dbus_message_iter_get_basic(&it, &s);
    snprintf(b, bn, "%s", s ? s : "");
    return true;
}

/* NameOwnerChanged carries (name, old_owner, new_owner). */
static bool
read_name_owner(DBusMessage *msg, char *name, size_t nn, char *old_owner,
    size_t on, char *new_owner, size_t wr)
{
    DBusMessageIter it;

    *name = *old_owner = *new_owner = '\0';
    if (!dbus_message_iter_init(msg, &it))
        return false;
    for (int i = 0; i < 3; i++) {
        const char *s = NULL;
        char *dst = i == 0 ? name : i == 1 ? old_owner : new_owner;
        size_t cap = i == 0 ? nn : i == 1 ? on : wr;

        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
            return false;
        dbus_message_iter_get_basic(&it, &s);
        snprintf(dst, cap, "%s", s ? s : "");
        if (i < 2 && !dbus_message_iter_next(&it))
            return false;
    }
    return true;
}

static void
method_get(DBusMessage *msg)
{
    char iface[128], name[128];
    DBusMessageIter out, var, arr;
    DBusMessage *reply;

    if (!read_str_args(msg, iface, sizeof(iface), name, sizeof(name))) {
        reply_error(msg, "org.freedesktop.DBus.Error.InvalidArgs");
        return;
    }
    if (*iface && strcmp(iface, WATCHER_IFACE)) {
        reply_error(msg, "org.freedesktop.DBus.Error.UnknownInterface");
        return;
    }
    if (!prop_known(name)) {
        reply_error(msg, "org.freedesktop.DBus.Error.UnknownProperty");
        return;
    }
    reply = dbus_message_new_method_return(msg);
    if (!reply)
        return;
    dbus_message_iter_init_append(reply, &out);
    {
        const char *sig = !strcmp(name, "IsStatusNotifierHostRegistered")
            ? "b" : !strcmp(name, "ProtocolVersion") ? "i" : "as";

        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT, sig, &var);
        if (sig[0] == 'b') {
            /* austere is always its own tray host, so this is true from
             * the moment the watcher name is owned. */
            dbus_bool_t yes = TRUE;

            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &yes);
        } else if (sig[0] == 'i') {
            int32_t v = 0;

            dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &v);
        } else {
            dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s",
                &arr);
            append_items(&arr);
            dbus_message_iter_close_container(&var, &arr);
        }
        dbus_message_iter_close_container(&out, &var);
    }
    send_msg(reply);
}

static void
method_get_all(DBusMessage *msg)
{
    static const char *names[] = {
        "IsStatusNotifierHostRegistered",
        "RegisteredStatusNotifierItems",
        "ProtocolVersion",
        NULL
    };
    char iface[128] = { 0 };
    DBusMessageIter out, arr, entry, var, list;
    DBusMessage *reply = dbus_message_new_method_return(msg);

    if (!reply)
        return;
    dbus_message_iter_init_append(reply, &out);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &arr);
    /* an unknown interface yields an empty dictionary, not an error */
    if (read_str_args(msg, iface, sizeof(iface), NULL, 0) &&
        (!*iface || !strcmp(iface, WATCHER_IFACE))) {
        for (unsigned i = 0; names[i]; i++) {
            const char *key = names[i];
            const char *sig = i == 0 ? "b" : i == 1 ? "as" : "i";

            dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY,
                NULL, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
                &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                sig, &var);
            if (sig[0] == 'b') {
                dbus_bool_t yes = TRUE;

                dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN,
                    &yes);
            } else if (sig[0] == 'i') {
                int32_t v = 0;

                dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &v);
            } else {
                dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY,
                    "s", &list);
                append_items(&list);
                dbus_message_iter_close_container(&var, &list);
            }
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&arr, &entry);
        }
    }
    dbus_message_iter_close_container(&out, &arr);
    send_msg(reply);
}

/* The registration argument comes in four shapes: a bare object path
 * (the sender is the service), "service/path" (what Electron and
 * libappindicator send), and a bare well-known or unique name (the path
 * defaults to /StatusNotifierItem). */
static bool
parse_registration(const char *arg, const char *sender, char *service,
    char *path)
{
    const char *slash;

    if (!arg || !*arg)
        return false;
    if (arg[0] == '/') {
        snprintf(service, TRAY_SERVICE_MAX, "%s", sender ? sender : "");
        snprintf(path, TRAY_PATH_MAX, "%s", arg);
    } else if ((slash = strchr(arg, '/')) != NULL) {
        size_t n = (size_t)(slash - arg);

        if (n == 0 || n >= TRAY_SERVICE_MAX)
            return false;
        memcpy(service, arg, n);
        service[n] = '\0';
        snprintf(path, TRAY_PATH_MAX, "%s", slash);
    } else {
        snprintf(service, TRAY_SERVICE_MAX, "%s", arg);
        snprintf(path, TRAY_PATH_MAX, "%s", ITEM_PATH_DEFAULT);
    }
    return *service && path[0] == '/';
}

static void
item_add_match(const char *service, const char *path)
{
    char rule[TRAY_RULE_MAX];
    size_t n = strlen(service) + strlen(path);

    /* a quote would break the rule's quoting; bus names and object paths
     * cannot contain one, so refuse rather than emit a broken rule */
    if (strchr(service, '\'') || strchr(path, '\''))
        return;
    if (n + 64 >= sizeof(rule))
        return;
    snprintf(rule, sizeof(rule), "type='signal',sender='%s',path='%s'",
        service, path);
    bus_add_match(rule);
}

static void
method_register_item(DBusMessage *msg)
{
    char arg[512] = { 0 };
    const char *sender = dbus_message_get_sender(msg);
    char service[TRAY_SERVICE_MAX], path[TRAY_PATH_MAX];
    ti_t *it = NULL;
    int kind;

    read_str_args(msg, arg, sizeof(arg), NULL, 0);
    /* Never answer success we cannot honour: a silent empty reply leaves
     * the client believing it is in the tray while nothing observes it,
     * and it will never retry. An error makes it retry (or fall back to
     * the real watcher), which is the interop-correct answer. */
    if (!watcher_owned) {
        fprintf(stderr, "austere: tray: refusing %s from %s: we do not own"
            " %s\n", arg[0] ? arg : "?", sender ? sender : "?",
            WATCHER_NAME);
        reply_error_msg(msg, "org.freedesktop.DBus.Error.ServiceUnknown",
            "this connection does not own org.kde.StatusNotifierWatcher");
        return;
    }
    if (!sender || !parse_registration(arg, sender, service, path)) {
        fprintf(stderr, "austere: tray: unparsable registration \"%s\"\n",
            arg);
        reply_error_msg(msg, "org.freedesktop.DBus.Error.InvalidArgs",
            "expected an object path, service/path, or a bus name");
        return;
    }
    /* the match must be in place before the item can emit to us */
    item_add_match(service, path);
    kind = item_add(service, path, sender, &it);
    if (kind == ITEM_FAILED) {
        reply_error_msg(msg, "org.freedesktop.DBus.Error.LimitsExceeded",
            "the tray registry is full");
        return;
    }
    /* a re-registration only refreshes the entry: the registry (and the
     * signals that describe it) must not grow a second time */
    if (kind == ITEM_NEW) {
        DBusMessage *sig;
        const char *id = it->id;
        DBusMessageIter sig_it;

        path_add(path);
        fprintf(stderr, "austere: tray: registered %s\n", id);
        sig = dbus_message_new_signal(WATCHER_PATH, WATCHER_IFACE,
            "StatusNotifierItemRegistered");
        if (sig) {
            dbus_message_iter_init_append(sig, &sig_it);
            dbus_message_iter_append_basic(&sig_it, DBUS_TYPE_STRING, &id);
            dbus_connection_send(bus, sig, NULL);
            dbus_message_unref(sig);
        }
        emit_items_changed();
    }
    reply_empty(msg);
}

static void
method_register_host(DBusMessage *msg)
{
    char arg[512] = { 0 };
    const char *sender = dbus_message_get_sender(msg);

    read_str_args(msg, arg, sizeof(arg), NULL, 0);
    if (!host_sig_sent) {
        host_sig_sent = true;
        send_msg(dbus_message_new_signal(WATCHER_PATH, WATCHER_IFACE,
            "StatusNotifierHostRegistered"));
    }
    fprintf(stderr, "austere: tray: host %s registered by %s\n",
        *arg ? arg : "?", sender ? sender : "?");
    reply_empty(msg);
}

static void
register_host_self(void)
{
    DBusMessage *m = dbus_message_new_method_call(WATCHER_NAME, WATCHER_PATH,
        WATCHER_IFACE, "RegisterStatusNotifierHost");
    const char *id = host_name;
    DBusMessageIter it;

    /* our own self-registration: the call travels back through our own
     * handler, and it is what any second watcher keys off */
    if (m) {
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &id);
    }
    send_msg(m);
}

/* ---- item properties ------------------------------------------------ */

static void
status_from_str(ti_t *it, const char *s)
{
    if (!s)
        return;
    if (!strcmp(s, "Passive"))
        it->status = TRAY_STATUS_PASSIVE;
    else if (!strcmp(s, "Active"))
        it->status = TRAY_STATUS_ACTIVE;
    else if (!strcmp(s, "NeedsAttention"))
        it->status = TRAY_STATUS_ATTENTION;
}

static void
str_value(DBusMessageIter *v, char *dst, size_t cap)
{
    int type = dbus_message_iter_get_arg_type(v);
    const char *s = NULL;

    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        return;
    dbus_message_iter_get_basic(v, &s);
    snprintf(dst, cap, "%s", s ? s : "");
}

static void
prop_apply(ti_t *it, const char *key, DBusMessageIter *v)
{
    if (!strcmp(key, "IconName"))
        str_value(v, it->icon_name, sizeof(it->icon_name));
    else if (!strcmp(key, "AttentionIconName"))
        str_value(v, it->attn_name, sizeof(it->attn_name));
    else if (!strcmp(key, "Title"))
        str_value(v, it->title, sizeof(it->title));
    else if (!strcmp(key, "Menu"))
        str_value(v, it->menu, sizeof(it->menu));
    else if (!strcmp(key, "Status")) {
        char buf[32];

        str_value(v, buf, sizeof(buf));
        status_from_str(it, buf);
    } else if (!strcmp(key, "ItemIsMenu")) {
        if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b = FALSE;

            dbus_message_iter_get_basic(v, &b);
            it->is_menu = b ? true : false;
        }
    } else if (!strcmp(key, "IconThemePath")) {
        char buf[TRAY_THEME_PATH_MAX];

        str_value(v, buf, sizeof(buf));
        /* a bad value clears the field rather than being kept: the icon
         * must never resolve against a path the loader would refuse */
        snprintf(it->theme_path, sizeof(it->theme_path), "%s",
            theme_path_ok(buf) ? buf : "");
    } else if (!strcmp(key, "IconPixmap")) {
        if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY)
            pixmap_parse(it, v);
    }
    /* ToolTip, OverlayIcon* and Category are accepted and ignored: they
     * are rendering concerns, not ones this backend owns. */
}

static void
props_apply(ti_t *it, DBusMessageIter *dict)
{
    DBusMessageIter e;

    dbus_message_iter_recurse(dict, &e);
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
            prop_apply(it, key, &v);
        }
        dbus_message_iter_next(&e);
    }
    icon_pick(it);
}

/* The SNI signals that mean "something changed, re-read me". NewMenu is
 * deliberately absent: the spec has no such signal (it belongs to
 * com.canonical.dbusmenu), and a Menu change arrives as a
 * PropertiesChanged on the item. */
static bool
signal_wants_refresh(const char *member)
{
    static const char *refresh[] = {
        "NewIcon", "NewAttentionIcon", "NewIconThemePath", "NewToolTip",
        "NewStatus", "NewTitle", "NewOverlayIcon", NULL
    };

    for (unsigned i = 0; refresh[i]; i++)
        if (!strcmp(member, refresh[i]))
            return true;
    return false;
}

static void
on_item_signal(ti_t *it, DBusMessage *msg)
{
    const char *member = dbus_message_get_member(msg);
    const char *iface = dbus_message_get_interface(msg);
    bool direct = false;

    if (!member)
        return;
    if (iface && !strcmp(iface, PROPS_IFACE) &&
        !strcmp(member, "PropertiesChanged")) {
        it->gen++;
        it->needs_refresh = true;
        view_dirty = true;
        return;
    }
    if (!signal_wants_refresh(member))
        return;
    /* NewStatus is the only one of these signals that carries a value, so
     * it is applied straight away; the rest are argument-less (NewTitle
     * included, per the spec) and only mean "ask again", which the
     * coalesced GetAll below does. */
    if (!strcmp(member, "NewStatus")) {
        DBusMessageIter in;

        if (dbus_message_iter_init(msg, &in) &&
            dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING) {
            char buf[32];

            str_value(&in, buf, sizeof(buf));
            status_from_str(it, buf);
            direct = true;
        }
    }
    it->gen++;
    it->needs_refresh = true;
    if (direct)
        icon_pick(it);
    view_dirty = true;
}

static void
on_name_owner_changed(DBusMessage *msg)
{
    char name[256], old_owner[256], new_owner[256];

    if (!read_name_owner(msg, name, sizeof(name), old_owner,
            sizeof(old_owner), new_owner, sizeof(new_owner)))
        return;

    if (!strcmp(name, WATCHER_NAME)) {
        if (*new_owner) {
            /* somebody else grabbed the name from under us */
            if (watcher_owned) {
                watcher_owned = false;
                fprintf(stderr, "austere: tray: %s taken over by %s;"
                    " the tray goes inert\n", WATCHER_NAME, new_owner);
            }
        } else if (!watcher_owned && bus_live()) {
            /* the other watcher went away: ask again from the next
             * pump, never from inside this signal handler */
            name_retry = true;
        }
        return;
    }
    for (unsigned i = 0; i < nitems; i++) {
        ti_t *it = &items[i];

        if (!*new_owner &&
            (!strcmp(name, it->owner) || !strcmp(name, it->service)))
            item_mark_dead(it);
    }
}

static void
on_reply(int kind, DBusMessage *msg)
{
    int type = dbus_message_get_type(msg);
    dbus_uint32_t code = 0;
    DBusMessageIter in;
    bool owned;

    if (type == DBUS_MESSAGE_TYPE_ERROR) {
        if (kind == PEND_NAME)
            fprintf(stderr, "austere: tray: cannot request %s: %s\n",
                WATCHER_NAME, dbus_message_get_error_name(msg) ?
                dbus_message_get_error_name(msg) : "error");
        return;
    }
    if (!dbus_message_iter_init(msg, &in) ||
        dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_UINT32)
        return;
    dbus_message_iter_get_basic(&in, &code);
    owned = code == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        code == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
    if (kind == PEND_HOST) {
        if (owned)
            register_host_self();
        return;
    }
    if (!owned) {
        fprintf(stderr, "austere: tray: %s is owned by another watcher"
            " (code %u) - the tray stays empty; stop it to let austere"
            " serve the tray\n", WATCHER_NAME, (unsigned)code);
        return;
    }
    watcher_owned = true;
    fprintf(stderr, "austere: tray: serving %s\n", WATCHER_NAME);
    request_name(host_name, PEND_HOST);
}

static void
on_props_reply(ti_t *it, DBusMessage *msg)
{
    DBusMessageIter in;
    const char *sender = dbus_message_get_sender(msg);

    it->pending = 0;
    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *err = dbus_message_get_error_name(msg);

        it->needs_refresh = false;
        /* a client that went away between registration and the fetch
         * must leave through the grace window, not linger as a ghost */
        if (err && (!strcmp(err, "org.freedesktop.DBus.Error."
                    "NameHasNoOwner") ||
                    !strcmp(err, "org.freedesktop.DBus.Error.ServiceUnknown")))
            item_mark_dead(it);
        return;
    }
    if (sender && *sender && strcmp(sender, it->owner))
        return;     /* a stale answer from a previous owner */
    if (dbus_message_iter_init(msg, &in))
        props_apply(it, &in);
    /* a signal that landed mid-flight must not be swallowed */
    it->needs_refresh = it->gen != it->gen_sent;
    view_dirty = true;
}

/* Collect whatever arrived. Only steal once the dispatch queue has
 * settled: a pending call completed mid-batch holds no reply yet, and
 * stealing then would be a libdbus assertion. */
static void
pend_harvest(void)
{
    if (!bus ||
        dbus_connection_get_dispatch_status(bus) != DBUS_DISPATCH_COMPLETE)
        return;
    for (unsigned i = 0; i < npends; ) {
        DBusPendingCall *pc = pends[i].pc;
        int kind = pends[i].kind;
        dbus_uint32_t serial = pends[i].serial;
        DBusMessage *reply;

        if (!dbus_pending_call_get_completed(pc)) {
            i++;
            continue;
        }
        reply = dbus_pending_call_steal_reply(pc);
        pends[i] = pends[npends - 1];
        npends--;
        dbus_pending_call_unref(pc);
        if (!reply)
            continue;
        if (kind == PEND_PROPS) {
            for (unsigned k = 0; k < nitems; k++) {
                if (items[k].pending != serial)
                    continue;
                on_props_reply(&items[k], reply);
                break;
            }
        } else if (kind >= TRAYINT_PEND_MENU_ABOUT)
            /* the menu client matches the reply by the serial it
             * recorded when it sent the call, and drops anything that
             * no longer belongs to the open menu */
            traymenu_dispatch(reply);
        else
            on_reply(kind, reply);
        dbus_message_unref(reply);
    }
}

static DBusHandlerResult
on_message(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    int type = dbus_message_get_type(msg);

    (void)conn;
    (void)user_data;

    /* replies to tracked calls are consumed by the pending-call table
     * before they ever reach a vtable; the rest of the replies (the
     * fire-and-forget AddMatch ones) are dropped here on purpose. */
    if (type != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
            const char *iface = dbus_message_get_interface(msg);
            const char *member = dbus_message_get_member(msg);

            if (iface && member && !strcmp(iface, DBUS_IFACE_FULL) &&
                !strcmp(member, "NameOwnerChanged"))
                on_name_owner_changed(msg);
            else if (traymenu_dispatch(msg)) {
                /* a LayoutUpdated or ItemsPropertiesUpdated on the open
                 * menu's path: taken, coalesced and refetched later */
            } else {
                ti_t *it = item_by_signal(msg);

                if (it)
                    on_item_signal(it, msg);
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, WATCHER_IFACE,
            "RegisterStatusNotifierItem")) {
        method_register_item(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, WATCHER_IFACE,
            "RegisterStatusNotifierHost")) {
        method_register_host(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, PROPS_IFACE, "Get")) {
        method_get(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, PROPS_IFACE, "GetAll")) {
        method_get_all(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, PROPS_IFACE, "Set")) {
        reply_error(msg, "org.freedesktop.DBus.Error.PropertyReadOnly");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, INTROSPECT_IFACE, "Introspect")) {
        method_introspect(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, PEER_IFACE, "Ping")) {
        reply_empty(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
tray_teardown(void)
{
    /* The view is about to go empty, and view_sync() can no longer notice
     * it (every entry point bails out on a dead bus), so raise the flag
     * here or the bar keeps drawing icons for items that no longer exist. */
    if (nview)
        render_pending = true;
    for (unsigned i = 0; i < nitems; i++)
        item_reset(&items[i], false);
    nitems = 0;
    nview = 0;
    view_dirty = false;
    if (bus)
        paths_clear();
    pend_clear();
    if (bus) {
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        bus = NULL;
    }
    watcher_owned = false;
    host_sig_sent = false;
    full_logged = false;
    name_retry = false;
}

/* ---- public surface -------------------------------------------------- */

void
tray_init(wm_t *wm)
{
    (void)wm;
    DBusError err;

    if (bus)
        return;
    dbus_error_init(&err);
    bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!bus) {
        fprintf(stderr, "austere: tray: no session bus: %s\n",
            err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    vtable.message_function = on_message;
    if (!dbus_connection_register_object_path(bus, WATCHER_PATH, &vtable,
            NULL)) {
        fprintf(stderr, "austere: tray: cannot export %s\n", WATCHER_PATH);
        dbus_error_free(&err);
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        bus = NULL;
        return;
    }
    /* the daemon's own signals arrive on its object path */
    if (!dbus_connection_register_object_path(bus, DBUS_PATH_DBUS, &vtable,
            NULL))
        fprintf(stderr, "austere: tray: no bus signal path, dying items"
            " may linger\n");
    snprintf(host_name, sizeof(host_name), "%s%ld", HOST_NAME_PREFIX,
        (long)getpid());
    /* two untracked round trips: every item's signals and the death
     * detection. The bus honours them without an answer. */
    bus_add_match("type='signal',sender='org.freedesktop.DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged'");
    bus_add_match("type='signal',interface='org.freedesktop.DBus.Properties'"
        ",member='PropertiesChanged'");
    request_name(WATCHER_NAME, PEND_NAME);
    dbus_error_free(&err);
}

void
tray_shutdown(wm_t *wm)
{
    (void)wm;
    tray_teardown();
    memset(view, 0, sizeof(view));
    nview = 0;
    view_sig = 0;
    view_dirty = false;
    render_pending = false;
    menu_hint_until = 0;
    icon_px = TRAY_DEFAULT_ICON_PX;
    icon_px_set = 0;
}

int
tray_fd(void)
{
    int fd = -1;

    /* A dead socket stays readable, so polling it would spin: a
     * disconnected connection hands back no descriptor at all. */
    if (!bus_live())
        return -1;
    if (!dbus_connection_get_unix_fd(bus, &fd))
        return -1;
    return fd;
}

static void
pump_io(void)
{
    /* One non-blocking read pass, then convert whatever it produced;
     * dbus_connection_read_write_dispatch() never settles here because
     * our own queued replies keep it returning true. */
    dbus_connection_read_write(bus, 0);
    while (dbus_connection_get_dispatch_status(bus) !=
           DBUS_DISPATCH_COMPLETE) {
        if (dbus_connection_dispatch(bus) != DBUS_DISPATCH_DATA_REMAINS)
            break;
    }
}

/* The work every entry point ends with: fire the due deadlines, ask for
 * whatever changed and publish the snapshot. The repaint is the caller's
 * job (tray_render_pending). The pump runs this too, not only
 * tray_tick(): a busy bus keeps poll() from ever timing out, and a dying
 * item must still leave on schedule. */
static void
pump_round(void)
{
    long long now = now_ms();

    for (unsigned i = 0; i < nitems; ) {
        ti_t *it = &items[i];

        if (it->dead && now >= it->grace_until) {
            item_remove(i);
            continue;
        }
        /* an unanswered fetch is retried instead of wedging the item */
        if (it->pending && now - it->pending_since > TRAY_FETCH_TIMEOUT_MS) {
            pend_drop_serial(it->pending, true);
            it->pending = 0;
            it->needs_refresh = true;
        }
        i++;
    }
    /* A menu call that is never answered must not hold a reply-table slot
     * forever. By now the client has already fallen back to the item's own
     * ContextMenu, and a table it cannot get into would start refusing the
     * item fetches too. Dropped from here, which is not inside a handler,
     * so cancelling is safe. */
    for (unsigned i = 0; i < npends; ) {
        if (pends[i].kind >= TRAYINT_PEND_MENU_ABOUT &&
            now - pends[i].since > TRAY_MENU_HINT_MS)
            pend_drop_serial(pends[i].serial, true);
        else
            i++;
    }
    props_flush();
    traymenu_round();
    view_sync();
}

void
tray_pump(wm_t *wm)
{
    (void)wm;
    if (!bus_live())
        return;
    pump_io();
    if (!dbus_connection_get_is_connected(bus)) {
        fprintf(stderr, "austere: tray: bus connection lost, tray off\n");
        tray_teardown();
        return;
    }
    pend_harvest();
    if (name_retry) {
        /* the previous watcher went away; the flag is cleared here so
         * no bus call is made from inside a signal handler */
        name_retry = false;
        request_name(WATCHER_NAME, PEND_NAME);
    }
    pump_round();
}

int
tray_timeout_ms(wm_t *wm)
{
    long long now;
    int best = -1;

    (void)wm;
    if (!bus_live())
        return -1;
    now = now_ms();
    for (unsigned i = 0; i < nitems; i++) {
        ti_t *it = &items[i];
        long long dl = -1, left;

        if (it->dead)
            dl = it->grace_until;
        else if (it->pending)
            dl = it->pending_since + TRAY_FETCH_TIMEOUT_MS;
        else if (it->needs_refresh)
            /* a GetAll the last round could not send (full pending table,
             * refused write) must be retried, so wake up for it instead
             * of stalling until the next unrelated bus traffic */
            dl = now + TRAY_REFRESH_RETRY_MS;
        if (dl < 0)
            continue;
        left = dl - now;
        if (left < 0)
            left = 0;
        if (best < 0 || left < best)
            best = (int)left;
    }
    /* a menu exchange in flight is waiting on a reply nobody may send, so
     * the loop has to be woken for its deadline even on a quiet bus */
    if (menu_hint_until) {
        long long left = menu_hint_until - now;

        if (left < 0)
            left = 0;
        if (best < 0 || left < best)
            best = (int)left;
    }
    return best;
}

void
tray_tick(wm_t *wm)
{
    (void)wm;
    if (!bus_live())
        return;
    pump_round();
}

bool
tray_render_pending(void)
{
    bool due = render_pending;

    render_pending = false;
    return due;
}

bool
tray_available(void)
{
    return bus_live() && watcher_owned;
}

void
tray_set_icon_size(wm_t *wm, unsigned px)
{
    (void)wm;
    if (!px || (icon_px_set && px == icon_px))
        return;
    icon_px = px;
    icon_px_set = 1;
    if (!bus_live())
        return;
    for (unsigned i = 0; i < nitems; i++)
        icon_pick(&items[i]);
    view_dirty = true;
    view_sync();
}

/* The Menu property names the com.canonical.dbusmenu object, which is not
 * necessarily the item's own path: a standard KDE item keeps its SNI at
 * /StatusNotifierItem and its menu at /MenuBar. The spec's sentinel for
 * "this item has no menu" is the literal /NO_DBUSMENU, and a value that is
 * not a usable object path is refused too, so a client cannot aim the
 * menu exchange at something arbitrary. Refusing here is what makes the
 * caller fall back to the item's own ContextMenu. */
static bool
menu_path_ok(const char *s)
{
    size_t n;

    if (!s)
        return false;
    n = strlen(s);
    if (n < 2 || n >= TRAY_MENU_MAX)  /* "/x" is the shortest object path */
        return false;
    if (s[0] != '/' || !strcmp(s, "/NO_DBUSMENU"))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

/* True when this item advertises a menu object we can address. */
static bool
item_menu_addressable(const ti_t *it)
{
    return menu_path_ok(it->menu);
}

/* Everything the menu client needs is copied out of the table before any
 * traffic: the item's own path, which is its identity and what its death
 * is matched against, the menu object it advertised, which is where the
 * exchange is actually addressed, and its IconThemePath, so a row icon is
 * looked for in the same theme the item's own artwork came from. A later
 * pump that moves or drops the table slot cannot redirect any of it. */
static bool
menu_start(const ti_t *it, int root_x, int root_y)
{
    traymenu_req_t req;

    if (!bus_live() || !item_menu_addressable(it))
        return false;
    req.service = it->service;
    req.path = it->path;
    req.menu_path = it->menu;
    req.theme_path = it->theme_path;
    req.root_x = root_x;
    req.root_y = root_y;
    if (!traymenu_open(&req))
        return false;
    /* armed only once the exchange really started, and it expires on its
     * own, so a refused open costs the loop nothing */
    menu_hint_until = now_ms() + TRAY_MENU_HINT_MS;
    return true;
}

void
tray_click(unsigned idx, unsigned btn, int root_x, int root_y)
{
    char service[TRAY_SERVICE_MAX], path[TRAY_PATH_MAX];
    const char *member = NULL;
    ti_t *it;

    if (!bus_live() || idx >= nview)
        return;
    it = &items[view_src[idx]];
    /* resolve the snapshot index to a stable identity before any bus
     * traffic: the table may move under us during a later pump */
    snprintf(service, sizeof(service), "%s", it->service);
    snprintf(path, sizeof(path), "%s", it->path);
    switch (btn) {
    case 1:     /* an ItemIsMenu item has no Activate of its own */
        if (it->is_menu) {
            if (!menu_start(it, root_x, root_y))
                member = "ContextMenu";
            return;
        }
        member = "Activate";
        break;
    case 2:
        member = "SecondaryActivate";
        break;
    case 3:
        /* the menu the item exports, or its own ContextMenu when it
         * advertises none we can address */
        if (menu_start(it, root_x, root_y))
            return;
        member = "ContextMenu";
        break;
    case 4:
        item_call_scroll(service, path, 1, false);
        return;
    case 5:
        item_call_scroll(service, path, -1, false);
        return;
    case 6:
        item_call_scroll(service, path, -1, true);
        return;
    case 7:
        item_call_scroll(service, path, 1, true);
        return;
    default:
        return;
    }
    item_call_xy(service, path, member, (int32_t)root_x, (int32_t)root_y);
}

void
tray_scroll(unsigned idx, int delta, bool horizontal)
{
    char service[TRAY_SERVICE_MAX], path[TRAY_PATH_MAX];

    if (!bus_live() || !delta || idx >= nview)
        return;
    snprintf(service, sizeof(service), "%s", items[view_src[idx]].service);
    snprintf(path, sizeof(path), "%s", items[view_src[idx]].path);
    /* the wire spells the axis lowercase and the delta signed, positive
     * meaning up (or right) */
    item_call_scroll(service, path, (int32_t)delta, horizontal);
}

bool
tray_menu_open(unsigned idx, int root_x, int root_y)
{
    ti_t *it;
    char service[TRAY_SERVICE_MAX], path[TRAY_PATH_MAX];

    if (idx >= nview)
        return false;
    it = &items[view_src[idx]];
    if (menu_start(it, root_x, root_y))
        return true;
    /* Refused, and the press still has to be answered. The tray module
     * calls this and returns without a second attempt, on the contract that
     * a false return has already been handled here, so an item that
     * advertises no menu we can address - /NO_DBUSMENU, or a path the
     * loader would refuse - gets its own ContextMenu from this function
     * rather than silence. Fire and forget, exactly like tray_click's. */
    if (!bus_live())
        return false;
    snprintf(service, sizeof(service), "%s", it->service);
    snprintf(path, sizeof(path), "%s", it->path);
    item_call_xy(service, path, "ContextMenu", (int32_t)root_x,
        (int32_t)root_y);
    return false;
}

#else /* AUSTERE_NO_DBUS */

void
tray_init(wm_t *wm)
{
    (void)wm;   /* no bus, no items, no watcher */
}

void
tray_shutdown(wm_t *wm)
{
    (void)wm;
    nview = 0;
}

int
tray_fd(void)
{
    return -1;
}

void
tray_pump(wm_t *wm)
{
    (void)wm;
}

int
tray_timeout_ms(wm_t *wm)
{
    (void)wm;
    return -1;
}

void
tray_tick(wm_t *wm)
{
    (void)wm;
}

bool
tray_available(void)
{
    return false;
}

void
tray_set_icon_size(wm_t *wm, unsigned px)
{
    (void)wm;
    (void)px;
}

bool
tray_render_pending(void)
{
    return false;
}

void
tray_click(unsigned idx, unsigned btn, int root_x, int root_y)
{
    (void)idx;
    (void)btn;
    (void)root_x;
    (void)root_y;
}

void
tray_scroll(unsigned idx, int delta, bool horizontal)
{
    (void)idx;
    (void)delta;
    (void)horizontal;
}

/* No bus, so there is no menu to open; the UI falls back to the item's
 * own ContextMenu, which is what an item with no internal menu does
 * anyway. */
bool
tray_menu_open(unsigned idx, int root_x, int root_y)
{
    (void)idx;
    (void)root_x;
    (void)root_y;
    return false;
}

#endif /* AUSTERE_NO_DBUS */

tray_view_t
tray_view(void)
{
    tray_view_t v = { NULL, 0 };

    if (nview)
        v.items = view;
    v.nitems = nview;
    return v;
}

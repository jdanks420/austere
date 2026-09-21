#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include "client.h"
#include "util.h"

void *
xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "austere: out of memory\n");
        exit(1);
    }
    return p;
}

char *
xstrdup(const char *s)
{
    char *d = xmalloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

void *
get_property(wm_t *wm, xcb_window_t win, xcb_atom_t prop, xcb_atom_t type,
    uint32_t expect_format, size_t *out_len)
{
    xcb_get_property_reply_t *r;
    void *data;

    r = xcb_get_property_reply(wm->conn,
        xcb_get_property(wm->conn, 0, win, prop, type, 0, 4096), NULL);
    if (!r || r->type != type || r->format != expect_format ||
        xcb_get_property_value_length(r) == 0) {
        free(r);
        return NULL;
    }
    *out_len = (size_t)xcb_get_property_value_length(r);
    data = xmalloc(*out_len + (expect_format == 8 ? 1 : 0));
    memcpy(data, xcb_get_property_value(r), *out_len);
    if (expect_format == 8)
        ((char *)data)[*out_len] = '\0';
    free(r);
    return data;
}

uint8_t *
get_string_property(wm_t *wm, xcb_window_t win, xcb_atom_t prop,
    size_t *out_len)
{
    size_t len;
    uint8_t *s = get_property(wm, win, prop, XCB_ATOM_STRING, 8, &len);

    if (!s && wm->atoms)
        s = get_property(wm, win, prop,
            ((atoms_t *)wm->atoms)->utf8_string, 8, &len);
    if (s)
        *out_len = len;
    return s;
}

bool
has_proto(wm_t *wm, xcb_window_t win, xcb_atom_t proto)
{
    atoms_t *a = wm->atoms;
    xcb_atom_t *protos;
    size_t n;
    bool found = false;

    if (!(protos = get_property(wm, win, a->wm_protocols, XCB_ATOM_ATOM, 32,
             &n)))
        return false;
    for (size_t i = 0; i < n / sizeof(xcb_atom_t); i++)
        if (protos[i] == proto)
            found = true;
    free(protos);
    return found;
}

/* ICCCM §4.1.2.5: WM_CLASS is "instance\0class\0". Returns the class
 * string, or the instance when the class field is absent. Caller frees;
 * NULL when the property is missing or degenerate. */
char *
wm_class(wm_t *wm, xcb_window_t win)
{
    size_t len = 0;
    char *raw = (char *)get_property(wm, win, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, &len);

    if (!raw || len < 2) {
        free(raw);
        return NULL;
    }
    size_t in = 0;

    while (in < len && raw[in])
        in++;
    const char *cls = raw;

    if (in + 1 < len)
        cls = raw + in + 1;
    char *out = xstrdup(cls);

    free(raw);
    return out;
}

void
raise_window(wm_t *wm, xcb_window_t win)
{
    uint32_t above = XCB_STACK_MODE_ABOVE;

    xcb_configure_window(wm->conn, win, XCB_CONFIG_WINDOW_STACK_MODE,
        &above);
}

/* ICCCM §4.2.4: polite close via WM_DELETE; brutal kill for windows
 * that refuse the protocol. */
void
client_close(wm_t *wm, client_t *c)
{
    atoms_t *a = wm->atoms;

    if (has_proto(wm, c->win, a->wm_delete_window)) {
        xcb_client_message_event_t ev = { 0 };

        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = c->win;
        ev.type = a->wm_protocols;
        ev.data.data32[0] = a->wm_delete_window;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(wm->conn, 0, c->win, 0, (const char *)&ev);
    } else {
        xcb_kill_client(wm->conn, c->win);
    }
}

void
spawn_async(char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
        return;
    if (pid == 0) {
        setsid();
        execvp(argv[0], argv);
        fprintf(stderr, "austere: exec %s failed\n", argv[0]);
        _exit(127);
    }
}

void
spawn_shell(const char *cmd)
{
    char *const argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
    spawn_async(argv);
}

int
mkdir_p(const char *dir)
{
    char tmp[512];

    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Ascending bubble-sort of a pointer array; ci selects a
 * case-insensitive comparison. */
void
sort_strs(char **v, unsigned n, bool ci)
{
    for (unsigned a = 0; a + 1 < n; a++)
        for (unsigned b = a + 1; b < n; b++) {
            int c = ci ? strcasecmp(v[a], v[b]) : strcmp(v[a], v[b]);

            if (c > 0) {
                char *t = v[a];

                v[a] = v[b];
                v[b] = t;
            }
        }
}

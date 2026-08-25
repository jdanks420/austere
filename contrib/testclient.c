#define _GNU_SOURCE

/* Minimal controllable X client for austere's smoke tests: sets proper
 * WM_NAME/WM_CLASS/WM_PROTOCOLS, honors WM_DELETE_WINDOW, exits on
 * destroy. Not linked into austere. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
        xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

int
main(int argc, char **argv)
{
    const char *name = "testclient";
    const char *cls = "Testclient";
    const char *inst = "testclient";
    int w = 500, h = 350;
    int at_x = 0, at_y = 0, positioned = 0;
    int min_w = 0, min_h = 0;
    unsigned long parent = 0;
    int urgent = 0;
    int urgent_after_ms = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "--class") && i + 1 < argc) {
            char *sep = strchr(argv[++i], ':');
            cls = argv[i];
            if (sep) {
                inst = sep + 1;
                cls = strndup(argv[i], (size_t)(sep - argv[i]));
            }
        } else if (!strcmp(argv[i], "--fixed") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &w, &h);
        } else if (!strcmp(argv[i], "--min") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &min_w, &min_h);
        } else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            sscanf(argv[++i], "%d,%d", &at_x, &at_y);
            positioned = 1;
        } else if (!strcmp(argv[i], "--transient") && i + 1 < argc) {
            parent = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--urgent")) {
            urgent = 1;
        } else if (!strcmp(argv[i], "--urgent-after") && i + 1 < argc) {
            urgent_after_ms = atoi(argv[++i]);
        } else {
            fprintf(stderr,
                "usage: %s [--name S] [--class C:I] [--fixed WxH] "
                "[--at X,Y] [--min WxH] [--transient 0xWIN] "
                "[--urgent] [--urgent-after MS]\n",
                argv[0]);
            return 2;
        }
    }

    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c))
        return 1;
    xcb_screen_t *scr =
        xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    xcb_window_t win = xcb_generate_id(c);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t vals[] = { 0x2a2a2a,
        XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE };
    static int stagger;
    stagger += 37;
    int px = positioned ? at_x : 40 + (stagger % 240);
    int py = positioned ? at_y : 40 + (stagger % 160);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, scr->root, px, py,
        (uint16_t)w, (uint16_t)h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        scr->root_visual, mask, vals);

    {
        xcb_size_hints_t sz = { 0 };
        if (positioned) {
            sz.flags |= XCB_ICCCM_SIZE_HINT_US_POSITION;
            sz.x = at_x;
            sz.y = at_y;
        }
        if (min_w > 0 && min_h > 0) {
            sz.flags |= XCB_ICCCM_SIZE_HINT_P_MIN_SIZE |
                XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
            sz.min_width = sz.max_width = min_w;
            sz.min_height = sz.max_height = min_h;
        }
        if (sz.flags)
            xcb_icccm_set_wm_normal_hints(c, win, &sz);
    }

    xcb_icccm_set_wm_name(c, win, XCB_ATOM_STRING, 8,
        (uint32_t)strlen(name), name);
    xcb_atom_t utf8 = intern(c, "UTF8_STRING");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win,
        intern(c, "_NET_WM_NAME"), utf8, 8, strlen(name), name);
    /* real pid: the WM's swallowing walks the ancestry (SPEC §5.9) */
    int pid = (int)getpid();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win,
        intern(c, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_icccm_set_wm_class(c, win,
        (uint16_t)(strlen(inst) + strlen(cls) + 2),
        (char[64]){ 0 });
    /* set_wm_class needs "inst\0cls\0": build manually */
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%c%s", inst, '\0', cls);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, win,
            XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
            (uint32_t)(strlen(inst) + strlen(cls) + 2), buf);
    }

    if (parent) {
        uint32_t p = (uint32_t)parent;
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, win,
            XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &p);
    }

    xcb_atom_t wm_protocols = intern(c, "WM_PROTOCOLS");
    xcb_atom_t wm_delete_window = intern(c, "WM_DELETE_WINDOW");
    xcb_icccm_set_wm_protocols(c, win, wm_protocols, 1, &wm_delete_window);

    xcb_map_window(c, win);
    xcb_flush(c);

    /* Post-map so the WM's PropertyNotify path sees the change; a client
     * born urgent while focused counts as attended (SPEC §5.6). */
    if (urgent_after_ms >= 0) {
        usleep((useconds_t)urgent_after_ms * 1000);
        urgent = 1;
    }
    if (urgent) {
        xcb_icccm_wm_hints_t h = { 0 };
        h.flags = XCB_ICCCM_WM_HINT_X_URGENCY | XCB_ICCCM_WM_HINT_INPUT;
        h.input = 1;
        xcb_icccm_set_wm_hints(c, win, &h);
    }
    xcb_flush(c);

    xcb_generic_event_t *ev;
    while ((ev = xcb_wait_for_event(c))) {
        uint8_t type = ev->response_type & 0x7f;
        int done = 0;
        if (type == XCB_CLIENT_MESSAGE) {
            xcb_client_message_event_t *cm =
                (xcb_client_message_event_t *)ev;
            if (cm->type == wm_protocols &&
                cm->data.data32[0] == wm_delete_window)
                done = 1;
        } else if (type == XCB_DESTROY_NOTIFY) {
            xcb_destroy_notify_event_t *dn =
                (xcb_destroy_notify_event_t *)ev;
            if (dn->window == win || dn->event == win)
                done = 1;
        }
        free(ev);
        if (done)
            break;
    }

    xcb_destroy_window(c, win);
    xcb_disconnect(c);
    return 0;
}

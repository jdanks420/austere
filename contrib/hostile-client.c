#define _DEFAULT_SOURCE

/* usage: hostile-client [-r repeats]
 * Malformed-client torture for the running WM (§10): garbage properties,
 * self-referential transient hints, huge names, rapid map/unmap loops.
 * Exits 0; the WM must survive all of it. */
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static xcb_connection_t *c;
static xcb_screen_t *scr;

static xcb_window_t
mkwin(int x, int y, unsigned w, unsigned h)
{
    xcb_window_t win = xcb_generate_id(c);

    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, scr->root, x, y, w,
        h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
        XCB_CW_BACK_PIXEL, (uint32_t[]){ 0x444444 });
    return win;
}

static void
garbage_props(xcb_window_t win)
{
    /* WM_NAME: 64 KiB of 0xFF bytes, no terminator */
    char *big = malloc(65536);

    memset(big, 0xff, 65536);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING, 8, 65536, big);
    /* WM_CLASS: single byte, no NUL pair */
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING, 8, 1, "x");
    /* WM_HINTS: flags claim fields past the end (short property) */
    long hints[3] = { 0x7ff, 1, 2 };

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_HINTS,
        XCB_ATOM_WM_HINTS, 32, 3, hints);
    /* WM_NORMAL_HINTS: bogus flags */
    long size[18] = { 0 };
    size[0] = 0xffff;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win,
        XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, size);
    /* _NET_WM_PID: zero */
    int zero = 0;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, 
        xcb_intern_atom_reply(c,
            xcb_intern_atom(c, 0, 11, "_NET_WM_PID"), NULL)->atom,
        XCB_ATOM_CARDINAL, 32, 1, &zero);
    /* WM_TRANSIENT_FOR: self */
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win,
        XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &win);
    free(big);
}

int
main(int argc, char **argv)
{
    int reps = argc > 2 && !strcmp(argv[1], "-r") ? atoi(argv[2]) : 1;

    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c))
        return 1;
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    for (int i = 0; i < reps; i++) {
        xcb_window_t w1 = mkwin(10, 10, 100, 100);
        xcb_window_t w2 = mkwin(20, 20, 200, 150);

        garbage_props(w1);
        garbage_props(w2);
        xcb_map_window(c, w1);
        xcb_map_window(c, w2);
        xcb_flush(c);
        nanosleep(&(struct timespec){0, 20000000}, NULL);
        /* rapid map/unmap churn */
        for (int k = 0; k < 25; k++) {
            xcb_unmap_window(c, k % 2 ? w1 : w2);
            xcb_flush(c);
            nanosleep(&(struct timespec){0, 2000000}, NULL);
            xcb_map_window(c, k % 2 ? w2 : w1);
            xcb_flush(c);
            nanosleep(&(struct timespec){0, 2000000}, NULL);
        }
        xcb_destroy_window(c, w1);
        xcb_destroy_window(c, w2);
        xcb_flush(c);
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    }
    /* round-trip so the WM has processed everything before we exit */
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
    xcb_disconnect(c);
    return 0;
}

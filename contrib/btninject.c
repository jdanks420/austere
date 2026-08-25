#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>

/* usage: btninject click <x> <y>
 *        btninject move|resize <x0> <y0> <x1> <y1>
 * move/resize drag with super held, interpolating motion so the WM sees
 * a realistic path. */

#define KEYSYM_SUPER_L 0xffeb
#define STEPS 8

static xcb_connection_t *c;
static xcb_screen_t *scr;

static void
fake(uint8_t type, uint8_t detail, int x, int y)
{
    xcb_test_fake_input(c, type, detail, 0, scr->root, (int16_t)x,
        (int16_t)y, 0);
    xcb_flush(c);
    usleep(20000);
}

int
main(int argc, char **argv)
{
    xcb_key_symbols_t *ks;
    xcb_keycode_t *skc;
    int x0, y0, x1 = 0, y1 = 0;

    if (argc != 4 && argc != 6) {
        fprintf(stderr,
            "usage: %s click <x> <y> | %s move|resize <x0> <y0> <x1> <y1>\n",
            argv[0], argv[0]);
        return 2;
    }

    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "btninject: cannot open display\n");
        return 1;
    }
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    ks = xcb_key_symbols_alloc(c);
    skc = xcb_key_symbols_get_keycode(ks, KEYSYM_SUPER_L);

    if (!strcmp(argv[1], "click") && argc == 4) {
        sscanf(argv[2], "%i", &x0);
        sscanf(argv[3], "%i", &y0);
        /* XTEST buttons fire at the CURRENT pointer position; move first */
        fake(XCB_MOTION_NOTIFY, 0, x0, y0);
        fake(XCB_BUTTON_PRESS, XCB_BUTTON_INDEX_1, x0, y0);
        fake(XCB_BUTTON_RELEASE, XCB_BUTTON_INDEX_1, x0, y0);
    } else if ((argc == 6) &&
        (!strcmp(argv[1], "move") || !strcmp(argv[1], "resize"))) {
        uint8_t btn =
            !strcmp(argv[1], "move") ? XCB_BUTTON_INDEX_1 : XCB_BUTTON_INDEX_3;

        sscanf(argv[2], "%i", &x0);
        sscanf(argv[3], "%i", &y0);
        sscanf(argv[4], "%i", &x1);
        sscanf(argv[5], "%i", &y1);

        fake(XCB_KEY_PRESS, skc ? *skc : 0, 0, 0);
        fake(XCB_MOTION_NOTIFY, 0, x0, y0);
        fake(XCB_BUTTON_PRESS, btn, x0, y0);
        for (int i = 1; i <= STEPS; i++)
            fake(XCB_MOTION_NOTIFY, 0, x0 + (x1 - x0) * i / STEPS,
                y0 + (y1 - y0) * i / STEPS);
        fake(XCB_BUTTON_RELEASE, btn, x1, y1);
        fake(XCB_KEY_RELEASE, skc ? *skc : 0, 0, 0);
    } else {
        fprintf(stderr, "btninject: bad arguments\n");
        return 2;
    }

    free(skc);
    xcb_key_symbols_free(ks);
    xcb_disconnect(c);
    return 0;
}

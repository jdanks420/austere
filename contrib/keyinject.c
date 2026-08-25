#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

/* usage: keyinject <mods-hex> <keycode>
 * mods-hex: X modifier mask (1=shift 2=lock 4=ctrl 8=mod1 ... 64=mod4).
 * Taps <keycode> while holding every modifier bound in the mask. */

int
main(int argc, char **argv)
{
    xcb_connection_t *c;
    xcb_screen_t *scr;
    xcb_get_modifier_mapping_reply_t *mm;
    unsigned long mods;
    int keycode;
    uint8_t *mk;

    if (argc != 3 || sscanf(argv[1], "%lx", &mods) != 1 ||
        sscanf(argv[2], "%i", &keycode) != 1 || keycode < 8 || keycode > 255) {
        fprintf(stderr, "usage: %s <mods-hex> <keycode>\n", argv[0]);
        return 2;
    }

    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "keyinject: cannot open display\n");
        return 1;
    }
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    mm = xcb_get_modifier_mapping_reply(c,
        xcb_get_modifier_mapping(c), NULL);
    if (!mm)
        return 1;
    mk = xcb_get_modifier_mapping_keycodes(mm);

    /* hold: for each set bit n, press first keycode of modifier index n */
    for (int n = 7; n >= 0; n--) {
        if (!((mods >> n) & 1))
            continue;
        for (int i = 0; i < mm->keycodes_per_modifier; i++) {
            uint8_t kc = mk[n * mm->keycodes_per_modifier + i];
            if (!kc)
                continue;
            xcb_test_fake_input(c, XCB_KEY_PRESS, kc, 0, scr->root, 0, 0, 0);
            break;
        }
    }

    xcb_flush(c);
    xcb_test_fake_input(c, XCB_KEY_PRESS, (uint8_t)keycode, 0, scr->root, 0,
        0, 0);
    xcb_test_fake_input(c, XCB_KEY_RELEASE, (uint8_t)keycode, 0, scr->root,
        0, 0, 0);
    xcb_flush(c);

    /* Round-trip so disconnect cannot race the server reading the
     * injected events (same hazard as monpoke). */
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));

    for (int n = 0; n < 8; n++) {
        if (!((mods >> n) & 1))
            continue;
        for (int i = 0; i < mm->keycodes_per_modifier; i++) {
            uint8_t kc = mk[n * mm->keycodes_per_modifier + i];
            if (!kc)
                continue;
            xcb_test_fake_input(c, XCB_KEY_RELEASE, kc, 0, scr->root, 0, 0,
                0);
        break;
    }
    }
    xcb_flush(c);
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));

    free(mm);
    xcb_disconnect(c);
    return 0;
}

/* usage: monpoke WxH+X+Y [WxH+X+Y]
 * Posts the _AUSTERE_TEST_MONITORS ClientMessage so a running austere
 * re-enumerates monitors without real RandR hardware. Max 2 rects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static int
parse(const char *s, int *x, int *y, unsigned *w, unsigned *h)
{
    return sscanf(s, "%ux%u+%d+%d", w, h, x, y) == 4;
}

int
main(int argc, char **argv)
{
    xcb_connection_t *c;
    xcb_screen_t *scr;
    xcb_intern_atom_reply_t *r;
    uint8_t buf[20] = { 0 };
    xcb_atom_t atom;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s WxH+X+Y [WxH+X+Y]\n", argv[0]);
        return 2;
    }
    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "monpoke: cannot open display\n");
        return 1;
    }
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const char atom_name[] = "_AUSTERE_TEST_MONITORS";

    r = xcb_intern_atom_reply(c,
        xcb_intern_atom(c, 0, sizeof(atom_name) - 1, atom_name), NULL);
    if (!r)
        return 1;
    atom = r->atom;
    free(r);
    if (!atom) {
        fprintf(stderr, "monpoke: austere not running?\n");
        return 1;
    }

    buf[0] = (uint8_t)(argc - 1);
    for (int i = 1; i < argc; i++) {
        int x, y;
        unsigned w, h;
        uint8_t *p = buf + 1 + 8 * (i - 1);

        if (!parse(argv[i], &x, &y, &w, &h)) {
            fprintf(stderr, "monpoke: bad rect '%s'\n", argv[i]);
            return 2;
        }
        memcpy(p, &(int16_t){ x }, 2);
        memcpy(p + 2, &(int16_t){ y }, 2);
        memcpy(p + 4, &(uint16_t){ w }, 2);
        memcpy(p + 6, &(uint16_t){ h }, 2);
    }

    xcb_client_message_event_t ev = { 0 };

    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.type = atom;
    ev.format = 8;
    ev.window = scr->root;
    memcpy(ev.data.data8, buf, 20);
    xcb_send_event(c, 0, scr->root,
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
        (const char *)&ev);
    xcb_flush(c);
    /* Disconnect can race the server reading the socket. */
    sleep(1);
    xcb_disconnect(c);
    return 0;
}

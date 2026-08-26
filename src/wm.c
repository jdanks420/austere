#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include "atoms.h"
#include "client.h"
#include "event.h"
#include "layout.h"
#include "bar.h"
#include "draw.h"
#include "ewmh.h"
#include "popup.h"
#include "settings.h"
#include "states.h"
#include "socket.h"
#include "state.h"
#include "welcome.h"
#include "hotwatch.h"
#include "keys.h"
#include "conf.h"
#include "workspace.h"
#include "wm.h"

static wm_t *g_wm;

static void
on_signal(int sig)
{
    if (!g_wm)
        return;
    int saved = errno;
    /* SIGHUP asks for a conf reload (§9.2); INT/TERM quit. The handler
     * only writes the self-pipe — nothing else is async-safe. */
    char b = sig == SIGHUP ? 'r' : 'w';

    if (sig != SIGHUP) {
        fprintf(stderr, "austere: signal %d quitting\n", sig);
        g_wm->running = 0;
    }
    ssize_t r;
    do {
        r = write(g_wm->pipe[1], &b, 1);
    } while (r < 0 && errno == EINTR);
    errno = saved;
}

static bool
window_alive(wm_t *wm, xcb_window_t win)
{
    xcb_get_window_attributes_reply_t *r =
        xcb_get_window_attributes_reply(wm->conn,
            xcb_get_window_attributes(wm->conn, win), NULL);
    if (!r)
        return false;
    free(r);
    return true;
}

static int
setup_signals(wm_t *wm)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        sigaction(SIGHUP, &sa, NULL) < 0)
        return -1;

    if (pipe2(wm->pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

/* §10 scan(): adopt pre-existing mapped windows after (re)start. */
static void
scan_existing(wm_t *wm)
{
    xcb_query_tree_cookie_t ck = xcb_query_tree(wm->conn,
        wm->scr->root);
    xcb_query_tree_reply_t *r = xcb_query_tree_reply(wm->conn, ck,
        NULL);

    if (!r)
        return;
    xcb_window_t *kids = xcb_query_tree_children(r);
    int n = xcb_query_tree_children_length(r);

    for (int i = 0; i < n; i++) {
        xcb_get_window_attributes_cookie_t ack =
            xcb_get_window_attributes(wm->conn, kids[i]);
        xcb_get_window_attributes_reply_t *a =
            xcb_get_window_attributes_reply(wm->conn, ack, NULL);

        if (!a)
            continue;
        bool mapped = a->map_state == XCB_MAP_STATE_VIEWABLE;
        bool oride = a->override_redirect;

        free(a);
        if (mapped && !oride && !find_client(wm, kids[i]))
            manage(wm, kids[i]);
    }
    free(r);
    arrange(wm);
    bar_render_all(wm);
}

static void
publish_identity(wm_t *wm)
{
    xcb_screen_t *s = wm->scr;
    atoms_t *a = wm->atoms;    const char name[] = "austere";

    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, s->root,
        a->net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm->mgr_win);
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->mgr_win,
        a->net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm->mgr_win);
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, wm->mgr_win,
        a->net_wm_name, a->utf8_string, 8,
        sizeof(name) - 1, name);
    xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, s->root,
        a->net_wm_name, a->utf8_string, 8, sizeof(name) - 1, name);
}

static void
announce_manager(wm_t *wm)
{
    atoms_t *a = wm->atoms;
    xcb_client_message_event_t ev = { 0 };

    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = wm->scr->root;
    ev.type = a->manager;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = a->wm_sn;
    ev.data.data32[2] = wm->mgr_win;

    xcb_send_event(wm->conn, 0, wm->scr->root,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY,
        (const char *)&ev);
}

static void
wait_old_owner_exit(wm_t *wm, xcb_window_t old)
{
    for (int i = 0; i < 50 && window_alive(wm, old); i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (window_alive(wm, old))
        fprintf(stderr,
            "austere: previous WM still alive after takeover; continuing\n");
}

static int
acquire_selection(wm_t *wm)
{
    atoms_t *a = wm->atoms;
    xcb_get_selection_owner_reply_t *own =
        xcb_get_selection_owner_reply(wm->conn,
            xcb_get_selection_owner(wm->conn, a->wm_sn), NULL);
    xcb_window_t old = own ? own->owner : XCB_NONE;
    free(own);

    if (old != XCB_NONE && !wm->replace) {
        fprintf(stderr, "austere: another window manager already owns "
                        "WM_S%d (use --replace to take over)\n",
            wm->scr_index);
        return -1;
    }

    uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[] = { 1, XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                XCB_EVENT_MASK_PROPERTY_CHANGE };
    wm->mgr_win = xcb_generate_id(wm->conn);
    xcb_create_window(wm->conn, XCB_COPY_FROM_PARENT, wm->mgr_win,
        wm->scr->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        wm->scr->root_visual, mask, vals);

    xcb_set_selection_owner(wm->conn, wm->mgr_win, a->wm_sn,
        XCB_CURRENT_TIME);

    own = xcb_get_selection_owner_reply(wm->conn,
        xcb_get_selection_owner(wm->conn, a->wm_sn), NULL);
    bool got = own && own->owner == wm->mgr_win;
    free(own);
    if (!got) {
        fprintf(stderr, "austere: failed to acquire %s\n", "WM selection");
        return -1;
    }

    announce_manager(wm);
    if (old != XCB_NONE)
        wait_old_owner_exit(wm, old);
    return 0;
}

static void
shutdown(wm_t *wm)
{
    socket_shutdown(wm);
    bars_shutdown(wm);
    popups_shutdown(wm);
    draw_shutdown(wm);
    ungrab_keys(wm);
    if (wm->mgr_win) {
        xcb_delete_property(wm->conn, wm->scr->root,
            wm->atoms->net_supporting_wm_check);
        xcb_destroy_window(wm->conn, wm->mgr_win);
    }
    workspaces_shutdown();
    monitors_shutdown(wm);
    xcb_flush(wm->conn);
    xcb_disconnect(wm->conn);
    xcb_key_symbols_free(wm->keysyms);
}

int
wm_main(int argc, char **argv)
{
    static wm_t wm;
    static atoms_t atoms;
    int preferred = 0;

    g_wm = &wm;
    wm.self = argc > 0 ? argv[0] : "austere";
    wm.argv = argv;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replace") == 0)
            wm.replace = true;
        else if (strcmp(argv[i], "--restart") == 0)
            wm.restarted = true;
        else {
            fprintf(stderr, "usage: %s [--replace] [--restart]\n",
                wm.self);
            return 2;
        }
    }

    wm.conn = xcb_connect(NULL, &preferred);
    if (xcb_connection_has_error(wm.conn)) {
        fprintf(stderr, "austere: cannot open display\n");
        return 1;
    }
    wm.scr_index = preferred;
    xcb_screen_iterator_t it =
        xcb_setup_roots_iterator(xcb_get_setup(wm.conn));
    for (int i = 0; i < preferred && it.rem > 1; i++)
        xcb_screen_next(&it);
    wm.scr = it.data;
    wm.atoms = &atoms;

    if (atoms_init(&atoms, wm.conn, wm.scr_index) < 0) {
        fprintf(stderr, "austere: atom intern failed\n");
        xcb_disconnect(wm.conn);
        return 1;
    }
    if (setup_signals(&wm) < 0) {
        perror("austere: signals/pipe");
        xcb_disconnect(wm.conn);
        return 1;
    }
    if (acquire_selection(&wm) < 0) {
        close(wm.pipe[0]);
        close(wm.pipe[1]);
        xcb_disconnect(wm.conn);
        return 1;
    }

    const uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_KEY_PRESS;
    xcb_change_window_attributes_checked(wm.conn, wm.scr->root,
        XCB_CW_EVENT_MASK, &root_mask);

    /* standard left_ptr arrow instead of the root default X glyph
     * (cursor font glyph 68 = XC_left_ptr; xcb has no XC_* constants) */
    xcb_font_t cfont = xcb_generate_id(wm.conn);

    xcb_open_font(wm.conn, cfont, 6, "cursor");
    xcb_cursor_t cursor = xcb_generate_id(wm.conn);

    xcb_create_glyph_cursor(wm.conn, cursor, cfont, cfont, 68, 69,
        0, 0, 0, 0xffff, 0xffff, 0xffff);
    xcb_change_window_attributes(wm.conn, wm.scr->root,
        XCB_CW_CURSOR, &cursor);
    xcb_close_font(wm.conn, cfont);

    publish_identity(&wm);
    settings_defaults(&cfg);
    keys_defaults(&cfg);
    conf_ensure(conf_path());
    {
        const char *state = states_boot_override();

        conf_load(state ? state : conf_path(), &cfg, false);
    }
    workspaces_init(&wm);
    ewmh_init(&wm);

    wm.randr_event_base = -1;
    const xcb_query_extension_reply_t *rext =
        xcb_get_extension_data(wm.conn, &xcb_randr_id);
    if (rext && rext->present) {
        wm.randr_event_base = rext->first_event;
        xcb_randr_select_input(wm.conn, wm.scr->root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
    }

    wm.keysyms = xcb_key_symbols_alloc(wm.conn);
    const char *env_layout = getenv("AUSTERE_LAYOUT");

    if (env_layout)
        set_layout(&wm, env_layout);
    else
        set_layout(&wm, cfg.default_layout);
    hotwatch_init(conf_path());
    socket_init(&wm);
    grab_keys(&wm);
    bar_init(&wm);
    popups_init(&wm);
    bars_sync(&wm);

    scan_existing(&wm);
    state_replay(&wm);

    if (!wm.restarted)
        for (unsigned i = 0; i < cfg.nautostart_run; i++)
            spawn_shell(cfg.autostart_run[i]);

    wm.running = 1;
    xcb_flush(wm.conn);
    welcome_maybe_show(&wm);
    event_loop(&wm);

    shutdown(&wm);
    return 0;
}

/* §10: serialize session, release the connection, exec ourselves.
 * The state file is consumed by scan/replay on the next boot. */
void
wm_restart(wm_t *wm)
{
    extern void state_save(wm_t *wm);

    state_save(wm);
    /* flag the re-exec so autostart only fires on fresh boots */
    int argc = 0;

    while (wm->argv[argc])
        argc++;
    const char **na = malloc((size_t)(argc + 2) * sizeof(*na));

    if (na) {
        int n = 0;

        while (wm->argv[n]) {
            na[n] = wm->argv[n];
            n++;
        }
        na[n++] = "--restart";
        na[n] = NULL;
        wm->argv = (char **)na;
    }
    fprintf(stderr, "austere: restarting in place\n");
    xcb_flush(wm->conn);
    xcb_disconnect(wm->conn);
    execvp(wm->self, wm->argv);
    fprintf(stderr, "austere: execvp failed: %s\n", strerror(errno));
    exit(1);
}

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include "client.h"
#include "actions.h"
#include "conf.h"
#include "bar.h"
#include "keys.h"
#include "launcher.h"
#include "menu.h"
#include "switcher.h"
#include "settings.h"
#include "states.h"
#include "socket.h"
#include "state.h"
#include "wallpaper.h"
#include "ewmh.h"
#include "apps.h"
#include "event.h"
#include "hotwatch.h"
#include "popup.h"
#include "layout.h"
#include "monitor.h"
#include "mouse.h"
#include "scratchpad.h"
#include "util.h"
#include "workspace.h"

#include <xcb/xcb_icccm.h>


static void
send_delete(wm_t *wm, client_t *c)
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
run_action(wm_t *wm, uint8_t action)
{
    if (action >= ACT_VIEW_WS && action < ACT_VIEW_WS + WS_MAX) {
        view_ws(wm, action - ACT_VIEW_WS);
        return;
    }
    if (action >= ACT_SEND_WS && action < ACT_SEND_WS + WS_MAX) {
        send_focused_to_ws(wm, action - ACT_SEND_WS);
        return;
    }
    switch (action) {
    case ACT_QUIT:
        wm->running = 0;
        break;
    case ACT_SPAWN_TERMINAL: {
        const char *env = getenv("AUSTERE_TERMINAL");

        spawn_shell(env ? env
                        : (cfg.terminal ? cfg.terminal : "xterm"));
        break;
    }
    case ACT_CLOSE_FOCUSED:
        if (wm->focused)
            send_delete(wm, wm->focused);
        break;
    case ACT_CYCLE_LAYOUT:
        cycle_layout(wm);
        break;
    case ACT_TOGGLE_FLOAT:
        toggle_float(wm);
        break;
    case ACT_MWFACT_DEC:
        adjust_mwfact(wm, -cfg.ratio_step);
        break;
    case ACT_MWFACT_INC:
        adjust_mwfact(wm, +cfg.ratio_step);
        break;
    case ACT_NMASTER_INC:
        adjust_nmaster(wm, +1);
        break;
    case ACT_NMASTER_DEC:
        adjust_nmaster(wm, -1);
        break;
    case ACT_RELOAD:
        settings_reload(wm);
        break;
    case ACT_MENU_SETTINGS:
            if (menu_active())
            menu_close(wm);
        else
            menu_open(wm);
        break;
    case ACT_SHOW_SWITCHER:
        if (!menu_active())
            switcher_panel_open(wm);
        break;
    case ACT_SHOW_LAUNCHER:
        if (!menu_active())
            launcher_open(wm);
        break;
    case ACT_MENU_APPS:
        if (!menu_active())
            menu_apps_open(wm);
        break;
    case ACT_MENU_STATES:
        if (!menu_active())
            menu_states_open(wm);
        break;
    case ACT_MRU_STEP:
        mru_step(wm);
        break;
    case ACT_WALLPAPER_RANDOM:
        wallpaper_random(wm);
        break;
    case ACT_WALLPAPER_NEXT:
        wallpaper_next(wm);
        break;
    case ACT_WALLPAPER_PICK:
        if (!menu_active())
            wallpaper_pick(wm);
        break;
    case ACT_RESTART: {
        extern void wm_restart(wm_t *wm);

        wm_restart(wm);
        break;
    }
    case ACT_WS_TO_NEXT_MON:
        ws_migrate_focused_to_next(wm);
        break;
    case ACT_TOGGLE_PREV_WS:
    {
        monitor_t *fm = focused_mon(wm);

        if (fm)
            view_ws(wm, fm->ws_visible);
    }
        break;
    case ACT_SCRATCH_TOGGLE:
        scratch_toggle(wm);
        break;
    case ACT_SCRATCH_MARK:
        scratch_mark(wm);
        break;
    default:
        break;
    }
}

static void
handle_key_press(wm_t *wm, xcb_key_press_event_t *ev)
{
    if (menu_active()) {
        menu_key(wm, ev);
        return;
    }
    unsigned state = ev->state & (XCB_MOD_MASK_SHIFT |
        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |
        XCB_MOD_MASK_4);

    for (unsigned i = 0; i < cfg.nbinds; i++) {
        if (cfg.binds[i].mods != state)
            continue;
        xcb_keycode_t *codes = xcb_key_symbols_get_keycode(wm->keysyms,
            cfg.binds[i].keysym);

        for (xcb_keycode_t *k = codes; k && *k; k++)
            if (*k == ev->detail) {
                free(codes);
                if (cfg.binds[i].action == ACT_VIEW_WS)
                    view_ws(wm, (unsigned)cfg.binds[i].arg);
                else if (cfg.binds[i].action == ACT_SEND_WS)
                    send_focused_to_ws(wm, (unsigned)cfg.binds[i].arg);
                else
                    run_action(wm, cfg.binds[i].action);
                return;
            }
        free(codes);
    }
}

static void
handle_map_request(wm_t *wm, xcb_map_request_event_t *ev)
{
    if (!find_client(wm, ev->window))
        manage(wm, ev->window);
}

static void
handle_unmap_notify(wm_t *wm, xcb_unmap_notify_event_t *ev)
{
    client_t *c = find_client(wm, ev->window);

    if (!c)
        return;
    /* dwm: only a client-initiated withdraw (synthetic unmap, §4.1.4)
     * unmanages. Our own hides and client glitches carry
     * send_event=false and are ignored. */
    if (ev->response_type & 0x80) { /* synthetic: client withdraw (§4.1.4) */
        long st = XCB_ICCCM_WM_STATE_WITHDRAWN;

        xcb_change_property(wm->conn, XCB_PROP_MODE_REPLACE, c->win,
            wm->atoms->wm_state, wm->atoms->wm_state, 32, 2,
            (long[]){ st, XCB_NONE });
        unmanage(wm, ev->window);
    }
}

static void
handle_destroy_notify(wm_t *wm, xcb_destroy_notify_event_t *ev)
{
    if (find_client(wm, ev->window))
        unmanage(wm, ev->window);
}

/* Tiled clients own their geometry: refuse the request but acknowledge per
 * ICCCM §4.1.5 so toolkits don't spin. Floats may place themselves. */
static void
handle_configure_request(wm_t *wm, xcb_configure_request_event_t *ev)
{
    client_t *c = find_client(wm, ev->window);
    uint32_t mask = ev->value_mask;
    uint32_t vals[5];
    int n = 0;

    if (!c || c->floating) {
        if (mask & XCB_CONFIG_WINDOW_X)
            vals[n++] = (uint32_t)ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y)
            vals[n++] = (uint32_t)ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)
            vals[n++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)
            vals[n++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
            vals[n++] = ev->border_width;
        if (n)
            xcb_configure_window(wm->conn, ev->window, mask, vals);

        if (c) {
            if (mask & XCB_CONFIG_WINDOW_X)
                c->x = ev->x;
            if (mask & XCB_CONFIG_WINDOW_Y)
                c->y = ev->y;
            if (mask & XCB_CONFIG_WINDOW_WIDTH)
                c->w = ev->width;
            if (mask & XCB_CONFIG_WINDOW_HEIGHT)
                c->h = ev->height;
        }
        return;
    }

    xcb_configure_notify_event_t sn = { 0 };
    sn.response_type = XCB_CONFIGURE_NOTIFY;
    sn.event = sn.window = c->win;
    sn.width = (uint16_t)c->w;
    sn.height = (uint16_t)c->h;
    xcb_send_event(wm->conn, 0, c->win, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
        (const char *)&sn);
}

/* Test seam: Xephyr cannot synthesize real RandR monitor changes, so
 * contrib/monpoke posts packed rect lists; production path is
 * monitors_refresh() on RandR events. Layout: u8 count, then per
 * monitor x,y,w,h as native-endian u16 pairs in data8[]. */
static void
apply_test_monitors(wm_t *wm, const xcb_client_message_event_t *ev)
{
    Rect rects[2];
    unsigned n = (unsigned char)ev->data.data8[0];

    if (n > 2)
        n = 2;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)ev->data.data8 + 1 + 8 * i;
        uint16_t x, y, w, h;

        memcpy(&x, p, 2);
        memcpy(&y, p + 2, 2);
        memcpy(&w, p + 4, 2);
        memcpy(&h, p + 6, 2);
        rects[i].x = (int16_t)x;
        rects[i].y = (int16_t)y;
        rects[i].w = w;
        rects[i].h = h;
    }
    monitors_apply(wm, rects, n);
    arrange(wm);
    if (wm->focused && !ws_shown(wm->focused->ws))
        refocus_ws(wm, workspaces[wm->focused->ws].mon->ws_visible);
}

static void
handle_randr(wm_t *wm)
{
    monitors_refresh(wm);
    arrange(wm);
    if (wm->focused && !ws_shown(wm->focused->ws))
        refocus_ws(wm, workspaces[wm->focused->ws].mon->ws_visible);
}

static void
handle_client_message(wm_t *wm, xcb_client_message_event_t *ev)
{
    atoms_t *a = wm->atoms;
    client_t *c;

    if (ev->type == a->austere_test_monitors) {
        apply_test_monitors(wm, ev);
        return;
    }
    if (ev->type == a->net_current_desktop) {
        view_ws(wm, ev->data.data32[0]);
        return;
    }
    if (ev->type == a->net_wm_desktop) {
        if ((c = find_client(wm, ev->window)) &&
            ev->data.data32[0] < WS_MAX)
            send_client_to_ws(wm, c, ev->data.data32[0]);
        return;
    }
    if (ev->type == a->net_close_window) {
        if ((c = find_client(wm, ev->window)))
            send_delete(wm, c);
        return;
    }
    if (ev->type == a->net_wm_state) {
        c = find_client(wm, ev->window);
        if (!c)
            return;
        unsigned action = ev->data.data32[0];

        if (ev->data.data32[1] == a->net_wm_state_fullscreen ||
            ev->data.data32[2] == a->net_wm_state_fullscreen) {
            bool on = action == 1
                ? true
                : action == 0 ? !c->fullscreen : false;

            if (on != c->fullscreen) {
                c->fullscreen = on;
                ewmh_set_fullscreen(wm, c, on);
                arrange(wm);
            }
        }
        return;
    }
    if (ev->type != a->wm_protocols)
        return;
    if (ev->data.data32[0] != (uint32_t)a->wm_delete_window)
        return;
    if ((c = find_client(wm, ev->window)))
        unmanage(wm, ev->window);
}

static void
handle_property_notify(wm_t *wm, xcb_property_notify_event_t *ev)
{
    atoms_t *a = wm->atoms;
    client_t *c;

    if (!(c = find_client(wm, ev->window)))
        return;
    if (ev->atom == a->wm_hints) {
        client_poll_urgency(wm, c);
        return;
    }
    if (ev->atom == a->net_wm_name || ev->atom == XCB_ATOM_WM_NAME) {
        client_refresh_name(wm, c);
        bar_render_all(wm);
    }
}

static void
handle_enter_notify(wm_t *wm, xcb_enter_notify_event_t *ev)
{
    client_t *c;

    if (ev->mode != XCB_NOTIFY_MODE_NORMAL ||
        ev->detail == XCB_NOTIFY_DETAIL_INFERIOR)
        return;
    if (!cfg.focus_follows_mouse)
        return;
    if ((c = find_client(wm, ev->event)))
        focus(wm, c);
}

static void
handle_event(wm_t *wm, xcb_generic_event_t *ev)
{
    switch (ev->response_type & 0x7f) {
    case XCB_KEY_PRESS:
        handle_key_press(wm, (xcb_key_press_event_t *)ev);
        break;
    case XCB_MAP_REQUEST:
        handle_map_request(wm, (xcb_map_request_event_t *)ev);
        break;
    case XCB_UNMAP_NOTIFY:
        handle_unmap_notify(wm, (xcb_unmap_notify_event_t *)ev);
        break;
    case XCB_DESTROY_NOTIFY:
        handle_destroy_notify(wm, (xcb_destroy_notify_event_t *)ev);
        break;
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request(wm, (xcb_configure_request_event_t *)ev);
        break;
    case XCB_CLIENT_MESSAGE:
        handle_client_message(wm, (xcb_client_message_event_t *)ev);
        break;
    case XCB_PROPERTY_NOTIFY:
        handle_property_notify(wm, (xcb_property_notify_event_t *)ev);
        break;
    case XCB_ENTER_NOTIFY:
        handle_enter_notify(wm, (xcb_enter_notify_event_t *)ev);
        break;
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t *bev =
            (xcb_button_press_event_t *)ev;

        if (menu_panel_button(wm, bev->event, bev->event_y,
                bev->detail))
            break;
        if (menu_overlay_button(wm, bev->event, bev->event_x,
                bev->event_y, bev->detail, bev->time))
            break;
        if (!bar_button(wm, bev->event, bev->event_x, bev->detail))
            mouse_press(wm, bev);
        break;
    }
    case XCB_EXPOSE:
        if (menu_owns_window(((xcb_expose_event_t *)ev)->window))
            menu_expose(wm);
        break;
    case XCB_MOTION_NOTIFY:
        mouse_motion(wm, (xcb_motion_notify_event_t *)ev);
        break;
    case XCB_BUTTON_RELEASE:
        mouse_release(wm, (xcb_button_release_event_t *)ev);
        break;
    case XCB_SELECTION_CLEAR:
        /* Another WM took over WM_Sn (--replace flow): leave politely.
         * Destroying our manager window releases the selection; never
         * SetSelectionOwner(None) here or we clobber the successor. */
        {
            xcb_selection_clear_event_t *sc =
                (xcb_selection_clear_event_t *)ev;
            if (sc->selection == wm->atoms->wm_sn) {
                fprintf(stderr, "austere: lost WM_Sn selection, exiting\n");
                wm->running = 0;
            }
        }
        break;
    default:
        if (wm->randr_event_base >= 0 &&
            ((ev->response_type & 0x7f) ==
                (unsigned)wm->randr_event_base ||
                (ev->response_type & 0x7f) ==
                    (unsigned)wm->randr_event_base + 1)) {
            handle_randr(wm);
            break;
        }
        if (ev->response_type == 0) {
            xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
            fprintf(stderr,
                "austere: X error op=%u code=%u seq=%u res=%u\n",
                err->major_code, err->error_code, err->sequence,
                err->resource_id);
        }
        break;
    }
}

void
event_loop(wm_t *wm)
{
    struct pollfd fds[2] = {
        { .fd = xcb_get_file_descriptor(wm->conn), .events = POLLIN },
        { .fd = wm->pipe[0], .events = POLLIN },
    };
    struct pollfd all[64];
    xcb_generic_event_t *ev;
    char buf[16];
    int hw_idx = -1;
    int sk_idx = -1;

    memcpy(all, fds, sizeof(fds));
    unsigned nfds = 2;

    while (wm->running) {
        while ((ev = xcb_poll_for_event(wm->conn))) {
            handle_event(wm, ev);
            free(ev);
            if (!wm->running)
                break;
        }

        if (!wm->running)
            break;

        /* Handlers queue new requests; they must reach the server before
         * we sleep, or the session appears frozen until a buffer fills. */
        xcb_flush(wm->conn);

        nfds = 2;
        bar_collect_fds(wm, all, &nfds, 61);
        int sk = socket_fd();

        if (sk >= 0 && nfds < 64) {
            all[nfds].fd = sk;
            all[nfds].events = POLLIN;
            sk_idx = (int)nfds;
            nfds++;
        } else
            sk_idx = -1;
        int hw = hotwatch_fd();

        if (hw >= 0 && nfds < 64) {
            all[nfds].fd = hw;
            all[nfds].events = POLLIN;
            hw_idx = (int)nfds;
            nfds++;
        } else
            hw_idx = -1;
        int timeout = bar_timeout_ms(wm);
        int pt = popups_timeout_ms(wm);
        int ht = hotwatch_timeout_ms();

        if (pt >= 0 && (timeout < 0 || pt < timeout))
            timeout = pt;
        if (ht >= 0 && (timeout < 0 || ht < timeout))
            timeout = ht;

        int r = poll(all, nfds, timeout);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "austere: poll error errno=%d\n", errno);
            wm->running = 0;
            break;
        }

        if (r == 0) {
            if (hotwatch_fire())
                settings_reload(wm);
            bar_render_all(wm); /* minute tick */
            popups_tick(wm);
            continue;
        }

        for (unsigned i = 2; i < nfds; i++) {
            if (all[i].revents & (POLLIN | POLLHUP)) {
                if ((int)i == hw_idx)
                    hotwatch_pump();
                else if ((int)i == sk_idx)
                    socket_handle(wm);
                else
                    bar_pump_fd(wm, all[i].fd);
            }
        }

        if (all[1].revents & POLLIN) {
            bool reload_req = false;

            while (read(wm->pipe[0], buf, sizeof(buf)) > 0)
                for (ssize_t i = 0; i < (ssize_t)sizeof(buf); i++)
                    if (buf[i] == 'r')
                        reload_req = true;
            if (reload_req)
                settings_reload(wm);
        }

        if (hotwatch_fire())
            settings_reload(wm);
    }
}

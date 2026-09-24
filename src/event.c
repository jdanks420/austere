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
#include "deco.h"
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
#include "notify.h"
#include "popup.h"
#include "layout.h"
#include "volume.h"
#include "monitor.h"
#include "mouse.h"
#include "scratchpad.h"
#include "util.h"
#include "workspace.h"

#include <xcb/xcb_icccm.h>


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
            client_close(wm, wm->focused);
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
    case ACT_RESTART:
        wm_restart(wm);
        break;
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
    case ACT_VIEW_PREV_WS:
        view_ws_prev(wm);
        break;
    case ACT_VIEW_NEXT_WS:
        view_ws_next(wm);
        break;
    case ACT_SCRATCH_TOGGLE:
        scratch_toggle(wm);
        break;
    case ACT_SCRATCH_MARK:
        scratch_mark(wm);
        break;
    case ACT_FULLSCREEN:
        if (wm->focused) {
            wm->focused->fullscreen = !wm->focused->fullscreen;
            ewmh_set_fullscreen(wm, wm->focused,
                wm->focused->fullscreen);
            arrange(wm);
        }
        break;
    case ACT_MAXIMIZE:
        if (wm->focused && wm->focused->deco)
            deco_toggle_maximize(wm, wm->focused);
        break;
    case ACT_RESTORE_MINIMIZED:
        deco_restore_minimized(wm);
        break;
    case ACT_TOGGLE_DECO:
        cfg.deco = !cfg.deco;
        deco_reconfigure_all(wm);
        arrange(wm);
        break;
    case ACT_FOCUS_LEFT:
        focus_direction(wm, FOCUS_LEFT);
        break;
    case ACT_FOCUS_RIGHT:
        focus_direction(wm, FOCUS_RIGHT);
        break;
    case ACT_FOCUS_UP:
        focus_direction(wm, FOCUS_UP);
        break;
    case ACT_FOCUS_DOWN:
        focus_direction(wm, FOCUS_DOWN);
        break;
    case ACT_EXEC:
        /* no command here: dispatch has the bind's cmd */
        break;
    case ACT_VOL_RAISE:
        volume_shift(5);
        break;
    case ACT_VOL_LOWER:
        volume_shift(-5);
        break;
    case ACT_VOL_MUTE:
        volume_toggle_mute();
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
        bool hit = false;

        for (unsigned k = 0; k < cfg.binds[i].ncodes; k++)
            if (cfg.binds[i].codes[k] == ev->detail) {
                hit = true;
                break;
            }
        if (!hit)
            continue;
        if (cfg.binds[i].action == ACT_VIEW_WS)
            view_ws(wm, (unsigned)cfg.binds[i].arg);
        else if (cfg.binds[i].action == ACT_SEND_WS)
            send_focused_to_ws(wm, (unsigned)cfg.binds[i].arg);
        else if (cfg.binds[i].action == ACT_EXEC)
            spawn_shell(cfg.binds[i].cmd);
        else if (cfg.binds[i].action == ACT_SET_LAYOUT)
            set_layout(wm, cfg.binds[i].cmd[0] ? cfg.binds[i].cmd
                                               : cfg.default_layout);
        else if (cfg.binds[i].action == ACT_LOAD_STATE)
            states_load(wm, cfg.binds[i].cmd);
        else
            run_action(wm, cfg.binds[i].action);
        return;
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
    client_t *c = find_client(wm, ev->window);

    if (c) {
        if (c->deco) {
            deco_cleanup(wm, c->deco);
            c->deco = NULL;
        }
        unmanage(wm, ev->window);
    } else if (menu_owns_window(ev->window)) {
        menu_window_gone(wm, ev->window);
    } else {
        client_t *c2 = find_client_by_deco(wm, ev->event);
        if (c2 && c2->deco) {
            deco_cleanup(wm, c2->deco);
            c2->deco = NULL;
            unmanage(wm, c2->win);
        }
    }
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

    if (!c)
        return;
    if (c->deco) {
        /* Reparented clients can't be placed by app coords (the wrapper
         * owns root position). Honor size changes in place. */
        unsigned w = (mask & XCB_CONFIG_WINDOW_WIDTH) ? ev->width : c->w;
        unsigned h = (mask & XCB_CONFIG_WINDOW_HEIGHT) ? ev->height : c->h;

        apply_geom(wm, c, c->x, c->y, w, h);
        return;
    }
    if (c->floating) {
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
        /* We place by explicit coords only; sibling/stack bits would
         * read beyond vals[] (we never fill slots 5/6). */
        mask &= ~(XCB_CONFIG_WINDOW_SIBLING |
            XCB_CONFIG_WINDOW_STACK_MODE);
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
            client_close(wm, c);
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
                : action == 2 ? !c->fullscreen : false;

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
    if (ev->atom == a->net_wm_state) {
        size_t slen = 0;
        xcb_atom_t *states = get_property(wm, c->win, a->net_wm_state,
            XCB_ATOM_ATOM, 32, &slen);
        bool fs = false;

        for (size_t i = 0; i < slen / sizeof(xcb_atom_t); i++)
            if (states[i] == a->net_wm_state_fullscreen) {
                fs = true;
                break;
            }
        free(states);
        if (fs != c->fullscreen) {
            c->fullscreen = fs;
            ewmh_set_fullscreen(wm, c, fs);
            arrange(wm);
        }
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
    case XCB_KEY_RELEASE:
        menu_key_release(wm, (xcb_key_release_event_t *)ev);
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
        if (deco_button_hit(wm, bev, bev->detail))
            break;
        if (popup_button(wm, bev->event, bev->event_x, bev->event_y,
                bev->detail))
            break;
        if (!bar_button(wm, bev->event, bev->event_x, bev->detail))
            mouse_press(wm, bev);
        break;
    }
    case XCB_EXPOSE:
        if (menu_owns_window(((xcb_expose_event_t *)ev)->window))
            menu_expose(wm);
        else if (popup_expose(wm, ((xcb_expose_event_t *)ev)->window))
            ;
        else if (find_client_by_deco(wm,
            ((xcb_expose_event_t *)ev)->window))
            deco_draw(wm, find_client_by_deco(wm,
                ((xcb_expose_event_t *)ev)->window));
        else
            bar_expose(wm, ((xcb_expose_event_t *)ev)->window);
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
    int sk_idx = -1;
    int nf_idx = -1;

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
        int nfd = notify_fd();

        if (nfd >= 0 && nfds < 64) {
            all[nfds].fd = nfd;
            all[nfds].events = POLLIN;
            nf_idx = (int)nfds;
            nfds++;
        } else
            nf_idx = -1;
        int timeout = bar_timeout_ms(wm);
        int pt = popups_timeout_ms(wm);

        if (pt >= 0 && (timeout < 0 || pt < timeout))
            timeout = pt;

        int r = poll(all, nfds, timeout);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "austere: poll error errno=%d\n", errno);
            wm->running = 0;
            break;
        }

        if (r == 0) {
            bar_render_all(wm); /* minute tick */
            popups_tick(wm);
            continue;
        }

        for (unsigned i = 2; i < nfds; i++) {
            if (all[i].revents & (POLLIN | POLLHUP)) {
                if ((int)i == sk_idx)
                    socket_handle(wm);
                else if ((int)i == nf_idx)
                    notify_pump(wm);
                else
                    bar_pump_fd(wm, all[i].fd);
            }
        }

        if (all[1].revents & POLLIN) {
            bool reload_req = false;

            ssize_t nread;
            while ((nread = read(wm->pipe[0], buf, sizeof(buf))) > 0)
                for (ssize_t i = 0; i < nread; i++)
                    if (buf[i] == 'r')
                        reload_req = true;
            if (reload_req)
                settings_reload(wm);
        }
    }
}

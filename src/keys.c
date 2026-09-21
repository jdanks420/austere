#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "actions.h"
#include "keys.h"
#include "util.h"
#include "workspace.h"

#include <xcb/xcb_keysyms.h>

/* Name→keysym for the binds grammar. xcb has no XStringToKeysym
 * equivalent and Xlib is off-limits, so the table covers everything
 * the default conf and common laptop keys use. */
static const struct {
    const char *name;
    xcb_keysym_t sym;
} keysym_tab[] = {
    { "Return", 0xff0d }, { "Tab", 0xff09 }, { "Escape", 0xff1b },
    { "Space", 0x0020 }, { "BackSpace", 0xff08 }, { "Delete", 0xffff },
    { "Insert", 0xff63 }, { "Home", 0xff50 }, { "End", 0xff57 },
    { "Prior", 0xff55 }, { "Next", 0xff56 },
    { "Left", 0xff51 }, { "Up", 0xff52 }, { "Right", 0xff53 },
    { "Down", 0xff54 },
    { "Print", 0xff61 }, { "Pause", 0xff13 },
    { "grave", 0x0060 }, { "minus", 0x002d }, { "equal", 0x003d },
    { "bracketleft", 0x005b }, { "bracketright", 0x005d },
    { "semicolon", 0x003b }, { "apostrophe", 0x0027 },
    { "comma", 0x002c }, { "period", 0x002e }, { "slash", 0x002f },
    { "backslash", 0x005c },
    { "XF86AudioRaiseVolume", 0x1008ff13 },
    { "XF86AudioLowerVolume", 0x1008ff11 },
    { "XF86AudioMute", 0x1008ff12 },
    { "XF86MonBrightnessUp", 0x1008ff02 },
    { "XF86MonBrightnessDown", 0x1008ff03 },
    { NULL, 0 },
};

static xcb_keysym_t
keysym_from_name(const char *s)
{
    /* single letters and digits are their own Latin-1 keysyms */
    if (((s[0] >= 'a' && s[0] <= 'z') ||
            (s[0] >= 'A' && s[0] <= 'Z') ||
            (s[0] >= '0' && s[0] <= '9')) &&
        !s[1])
        return (unsigned char)s[0];
    if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9' && !s[2]) {
        int n = s[1] - '1';

        return 0xffbe + n; /* F1..F9 */
    }
    if (s[0] == 'F' && s[1] == '1' && s[2] >= '0' && s[2] <= '2' &&
        !s[3])
        return 0xffbe + (s[2] - '0') + 9; /* F10..F12 */
    for (int i = 0; keysym_tab[i].name; i++)
        if (!strcasecmp(keysym_tab[i].name, s))
            return keysym_tab[i].sym;
    /* keysym_name() prints off-table keysyms as decimal; parse that
     * back so a menu-captured NumLock or KP or dead-key bind survives
     * save-reload instead of aborting the whole file. */
    if (*s >= '0' && *s <= '9') {
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 10);

        if (end && *end == '\0' && v <= 0x10ffff && v != 0)
            return (xcb_keysym_t)v;
    }
    return 0;
}

void
keysym_name(xcb_keysym_t sym, char *buf, unsigned bufsz)
{
    if ((sym >= 'a' && sym <= 'z') || (sym >= 'A' && sym <= 'Z') ||
        (sym >= '0' && sym <= '9')) {
        buf[0] = (char)sym;
        buf[1] = '\0';
        return;
    }
    if (sym >= 0xffbe && sym <= 0xffc9) {
        snprintf(buf, bufsz, "F%u", (unsigned)(sym - 0xffbe + 1));
        return;
    }
    for (int i = 0; keysym_tab[i].name; i++)
        if (keysym_tab[i].sym == sym) {
            snprintf(buf, bufsz, "%s", keysym_tab[i].name);
            return;
        }
    snprintf(buf, bufsz, "%u", (unsigned)sym);
}

unsigned
mod_from_name(const char *name, bool *ok)
{
    static const struct {
        const char *name;
        unsigned mask;
    } mods[] = {
        { "super", XCB_MOD_MASK_4 }, { "mod4", XCB_MOD_MASK_4 },
        { "alt", XCB_MOD_MASK_1 }, { "mod1", XCB_MOD_MASK_1 },
        { "ctrl", XCB_MOD_MASK_CONTROL },
        { "control", XCB_MOD_MASK_CONTROL },
        { "shift", XCB_MOD_MASK_SHIFT }, { "lock", XCB_MOD_MASK_LOCK },
        { "mod2", XCB_MOD_MASK_2 }, { "mod3", XCB_MOD_MASK_3 },
        { "mod5", XCB_MOD_MASK_5 },
        { NULL, 0 },
    };

    *ok = true;
    for (int i = 0; mods[i].name; i++)
        if (!strcmp(mods[i].name, name))
            return mods[i].mask;
    *ok = false;
    return 0;
}

/* "super+ctrl+Return" [action]: fills out->mods/keysym. The action id
 * is resolved by the caller (ws actions append the index). */
bool
parse_bind(const char *combo, const char *action, bind_t *out)
{
    char buf[128];
    bool ok;
    unsigned mods = 0;

    snprintf(buf, sizeof(buf), "%s", combo);
    char *last = strrchr(buf, '+');
    const char *keyname;

    if (last) {
        *last = '\0';
        keyname = last + 1;
        for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+"))
            mods |= mod_from_name(tok, &ok);
    } else {
        keyname = buf;
    }
    xcb_keysym_t sym = keysym_from_name(keyname);

    if (!sym)
        return false;

    uint8_t id = action_lookup(action);

    if (id == 255)
        return false;
    out->mods = mods;
    out->keysym = sym;
    out->action = id;
    out->arg = -1;
    out->cmd[0] = '\0';
    out->ncodes = 0; /* grab_keys resolves from keysym before dispatch */
    return true;
}

/* Compiled-in defaults mirror the historical bindings so existing
 * muscle memory stays stable. */
void
keys_defaults(settings_t *s)
{
    static const struct {
        const char *combo;
        const char *action;
    } rows[] = {
        { "super+m", "quit" },
        { "super+Return", "spawn_terminal" },
        { "alt+q", "close_focused" },
        { "super+s", "cycle_layout" },
        { "super+f", "toggle_fullscreen" },
        { "super+h", "ratio_shrink" },
        { "super+l", "ratio_grow" },
        { "alt+Tab", "show_switcher" },
        { "alt+shift+Tab", "show_switcher" },
        { "super+w", "show_switcher" },
        { "super+Tab", "mru_step" },
        { "alt+space", "show_launcher" },
        { "super+grave", "menu_states" },
        { "super+p", "scratch_toggle" },
        { "super+shift+s", "scratch_mark" },
        { "super+g", "toggle_float" },
        { "super+ctrl+m", "ws_to_monitor" },
        { "super+d", "menu_apps" },
        { "super+e", "menu_settings" },
        { "super+Escape", "reload" },
        { "super+shift+r", "restart" },
        { "alt+r", "wallpaper_random" },
        { "alt+w", "wallpaper_pick" },
        { "alt+Left", "focus_left" },
        { "alt+Right", "focus_right" },
        { "alt+Up", "focus_up" },
        { "alt+Down", "focus_down" },
        { "XF86AudioRaiseVolume", "volume_raise" },
        { "XF86AudioLowerVolume", "volume_lower" },
        { "XF86AudioMute", "volume_mute" },
    };

    s->nbinds = 0;
    for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        bind_t b;

        if (parse_bind(rows[r].combo, rows[r].action, &b) &&
            s->nbinds < MAX_BINDS)
            s->binds[s->nbinds++] = b;
    }
    for (unsigned d = 0; d < WS_MAX && s->nbinds + 1 < MAX_BINDS; d++) {
        bind_t b = { .mods = XCB_MOD_MASK_4, .keysym = (xcb_keysym_t)('1' + d),
            .action = ACT_VIEW_WS, .arg = (int)d };
        bind_t sb = { .mods = XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT,
            .keysym = (xcb_keysym_t)('1' + d), .action = ACT_SEND_WS,
            .arg = (int)d };

        s->binds[s->nbinds++] = b;
        s->binds[s->nbinds++] = sb;
    }
}

/* Resolve a bind's keysym to concrete keycodes and cache them. The
 * symbols table is a server snapshot; refreshing here (every grab_keys
 * — boot, reload, menu re-bind) keeps the cache in lockstep with the
 * grabs. */
static void
bind_resolve_keycodes(wm_t *wm, bind_t *b)
{
    xcb_keycode_t *codes = xcb_key_symbols_get_keycode(wm->keysyms,
        b->keysym);
    unsigned n = 0;

    for (xcb_keycode_t *k = codes; k && *k && n < 8; k++)
        b->codes[n++] = *k;
    b->ncodes = (uint8_t)n;
    free(codes);
}

void
grab_keys(wm_t *wm)
{
    for (unsigned i = 0; i < cfg.nbinds; i++) {
        bind_t *b = &cfg.binds[i];

        bind_resolve_keycodes(wm, b);
        for (unsigned k = 0; k < b->ncodes; k++)
            xcb_grab_key(wm->conn, 1, wm->scr->root, b->mods,
                b->codes[k], XCB_GRAB_MODE_ASYNC,
                XCB_GRAB_MODE_ASYNC);
    }
}

void
ungrab_keys(wm_t *wm)
{
    xcb_ungrab_key(wm->conn, XCB_GRAB_ANY, wm->scr->root, XCB_GRAB_ANY);
}

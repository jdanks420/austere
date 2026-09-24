#!/usr/bin/env python3
"""Regenerate the bundled color-theme config states (states/*.toml).

Each theme is the stock first-run configuration (the same sections and
keys conf.c writes) with only the palette swapped, so picking one from
the super+grave state picker is always a complete, valid state. Binds,
modules and geometry stay at the factory defaults; only colors vary.

Add a theme by appending THEMES below and rerunning. Palettes are each
scheme's canonical values (see "Palette notes").
"""

import os

# Palette notes:
#   tokyo-night / storm: folke's Tokyo Night {normal, storm}
#   gruvbox: morhetz gruvbox {dark, light}
#   catppuccin: latte/frappe/macchiato/mocha (canonical hex)
#   nord: arcticstudio nord (polar night + frost)
#   dracula: dracula pallet
#   solarized: ethanschoonover solarized {dark, light}
#   everforest: saint-lascive's everforest {dark, light}
#   one-dark: atom's one dark
#   monokai: monokai (sublime)
#   rose-pine: rose-pine {main, moon, dawn}
#   ayu: ayu {dark, mirage}
#   kanagawa: rebelot's kanagawa (wave)
#   zenburn: zenburn
#   github-dark: github's dark colorblind-friendly base

THEMES = [
    # stem                 desc                        focus    unfocus  urgent   bar_bg   bar_fg   deco_border  deco_unfocus
    ("tokyo-night",        "Tokyo Night (deep blue)",   "7aa2f7", "292e42", "f7768e", "1a1b26", "c0caf5", "3b4261", "1a1b26"),
    ("tokyo-night-storm",  "Tokyo Night Storm",        "7aa2f7", "3b4261", "f7768e", "24283b", "c0caf5", "3b4261", "1a1b26"),
    ("gruvbox-dark",       "Gruvbox dark (warm)",      "83a598", "3c3836", "fb4934", "282828", "ebdbb2", "504945", "282828"),
    ("gruvbox-light",      "Gruvbox light (cream)",    "83a598", "d5c4a1", "cc241d", "fbf1c7", "3c3836", "d5c4a1", "fbf1c7"),
    ("catppuccin-mocha",   "Catppuccin Mocha",         "89b4fa", "313244", "f38ba8", "1e1e2e", "cdd6f4", "45475a", "1e1e2e"),
    ("catppuccin-macchiato", "Catppuccin Macchiato",   "8aadf4", "363a4f", "ed8796", "24273a", "cad3f5", "494d64", "24273a"),
    ("catppuccin-frappe",  "Catppuccin Frappe",        "8caaee", "414559", "e78284", "303446", "c6d0f5", "51576d", "303446"),
    ("catppuccin-latte",   "Catppuccin Latte",         "1e66f5", "ccd0da", "d20f39", "eff1f5", "4c4f69", "bcc0cc", "eff1f5"),
    ("nord",               "Nord (polar night)",       "88c0d0", "3b4252", "bf616a", "2e3440", "d8dee9", "4c566a", "2e3440"),
    ("dracula",            "Dracula",                  "bd93f9", "343746", "ff5555", "282a36", "f8f8f2", "44475a", "282a36"),
    ("solarized-dark",     "Solarized dark",           "2aa198", "073642", "dc322f", "002b36", "839496", "073642", "002b36"),
    ("solarized-light",    "Solarized light",          "2aa198", "eee8d5", "dc322f", "fdf6e3", "657b83", "eee8d5", "fdf6e3"),
    ("everforest-dark",    "Everforest dark",          "7fbbb3", "3d484d", "e67e80", "2d353b", "d3c6aa", "4f5b57", "2d353b"),
    ("everforest-light",   "Everforest light",         "7fbbb3", "dfd8c4", "e67e80", "f4edd8", "5c6a72", "cfc6ae", "f4edd8"),
    ("one-dark",           "One Dark",                 "61afef", "2c323c", "e06c75", "282c34", "abb2bf", "3e4451", "282c34"),
    ("monokai",            "Monokai",                  "66d9ef", "373831", "f92672", "272822", "f8f8f2", "4d4a3e", "272822"),
    ("rose-pine",          "Rose Pine",                "c4a7e7", "26233a", "eb6f92", "191724", "e0def4", "403d52", "191724"),
    ("rose-pine-moon",     "Rose Pine Moon",           "c4a7e7", "2a283f", "eb6f92", "232136", "e0def4", "44415a", "232136"),
    ("rose-pine-dawn",     "Rose Pine Dawn",           "907aa9", "f2e9e1", "b4637a", "faf4ed", "575279", "dfdad0", "faf4ed"),
    ("ayu-dark",           "Ayu dark",                 "e6b450", "1a1f24", "f07178", "0f1419", "bfbdb6", "1c242b", "0f1419"),
    ("ayu-mirage",         "Ayu Mirage",               "ffcc66", "2a3141", "f28779", "1f2430", "cbccc6", "333a49", "1f2430"),
    ("kanagawa",           "Kanagawa (wave)",          "7e9cd8", "2b2b34", "c34043", "1f1f28", "dcd7ba", "363646", "1f1f28"),
    ("zenburn",            "Zenburn",                  "8cd0d3", "4d4d4d", "cc9393", "3f3f3f", "dcdccc", "5b5b5b", "3f3f3f"),
    ("github-dark",        "GitHub dark",              "58a6ff", "161b22", "f85149", "0d1117", "c9d1d9", "21262d", "0d1117"),
]

# Compiled-in defaults, kept in sync with src/keys.c keys_defaults().
CANONICAL_BINDS = [
    ("super+m", "quit"),
    ("super+Return", "spawn_terminal"),
    ("alt+q", "close_focused"),
    ("super+s", "cycle_layout"),
    ("super+f", "toggle_fullscreen"),
    ("super+h", "ratio_shrink"),
    ("super+l", "ratio_grow"),
    ("alt+Tab", "show_switcher"),
    ("alt+shift+Tab", "show_switcher"),
    ("super+w", "show_switcher"),
    ("super+Tab", "mru_step"),
    ("alt+space", "show_launcher"),
    ("super+grave", "menu_states"),
    ("super+p", "scratch_toggle"),
    ("super+shift+s", "scratch_mark"),
    ("super+g", "toggle_float"),
    ("super+ctrl+m", "ws_to_monitor"),
    ("super+d", "menu_apps"),
    ("super+e", "menu_settings"),
    ("super+Escape", "reload"),
    ("super+shift+r", "restart"),
    ("alt+r", "wallpaper_random"),
    ("alt+w", "wallpaper_pick"),
    ("alt+Left", "focus_left"),
    ("alt+Right", "focus_right"),
    ("alt+Up", "focus_up"),
    ("alt+Down", "focus_down"),
    ("super+Left", "view_ws_prev"),
    ("super+Right", "view_ws_next"),
    ("XF86AudioRaiseVolume", "volume_raise"),
    ("XF86AudioLowerVolume", "volume_lower"),
    ("XF86AudioMute", "volume_mute"),
]


def bind_block():
    lines = [f'    "{c} {a}",' for (c, a) in CANONICAL_BINDS]
    for d in range(9):
        lines.append(f'    "super+{d + 1} view_ws {d}",')
        lines.append(f'    "super+shift+{d + 1} send_ws {d}",')
    return "\n".join(lines)


def write_theme(outdir, stem, desc, focus, unfocus, urgent,
                bar_bg, bar_fg, deco_border, deco_unfocus):
    body = f"""# austere configuration
# {stem}.toml: {desc}.

[general]
terminal = "kitty"
socket = true

[appearance]
border_width = 2
focus_color = "#{focus}"
unfocus_color = "#{unfocus}"
urgent_color = "#{urgent}"
gap = 0
smart_gaps = false
corner_radius = 0
font = "Agave Nerd Font Mono:pixelsize=16"

[deco]
deco = false
deco_title_h = 20
deco_border = "#{deco_border}"
deco_unfocus_border = "#{deco_unfocus}"
# buttons = ["", "", ""]       # minimize, maximize, close glyphs

[bar]
position = "top"
time_format = "%a %d %b %H:%M"
bar_bg = "#{bar_bg}"
bar_fg = "#{bar_fg}"
bar_gap = 0
modules_left = ["workspaces", "layout"]
modules_center = ["title"]
modules_right = ["cpu", "ram", "battery", "volume", "clock"]

[behavior]
focus_follows_mouse = true
raise_on_click = true
snap_distance = 12
popup_timeout = 5
swallowing = false

[layouts]
default = "tile"
nmaster = 1
split_ratio = 0.50
ratio_step = 0.05

[workspaces]
names = ["", "", "", "", "", "", "", "", ""]

[keys]
bind = [
{bind_block()}
]

[mouse]
modifier = "super"
move_button = 1
resize_button = 3

[switcher]
scope = "all"

[launcher]
scan_path = true
history_size = 20

[wallpaper]
dirs = []
setter_command = "feh --bg-scale %s"

[autostart]
run = ["mate-polkit"]
"""
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, f"{stem}.toml")
    with open(path, "w") as f:
        f.write(body)
    return path


if __name__ == "__main__":
    import sys

    outdir = sys.argv[1] if len(sys.argv) > 1 else "states"
    for row in THEMES:
        print(write_theme(outdir, *row))
    print(f"{len(THEMES)} themes -> {outdir}")
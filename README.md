# austere

<p align="center"><img src="logo_black.svg" alt="austere" width="96"></p>

A low-spec X11 desktop shell: tiling/stacking/floating window manager,
modular status bar, and runtime settings menu — in C11 against raw
[libxcb](https://cgit.freedesktop.org/xcb/libxcb). No toolkits, no
cairo, no pango — text via fontconfig + freetype, one thread, one
connection, one `poll()` loop.

## Installation

### Dependencies

| library | purpose | Debian / Ubuntu | Arch | Fedora |
|---|---|---|---|---|
| libxcb | X11 client library | `libxcb1-dev` | `libxcb` | `libxcb-devel` |
| xcb-util-keysyms | keycode ↔ keysym tables | `libxcb-keysyms1-dev` | `xcb-util-keysyms` | `xcb-util-keysyms-devel` |
| xcb-util-icccm | EWMH / ICCCM helpers | `libxcb-icccm4-dev` | `xcb-util-wm` | `xcb-util-wm-devel` |
| xcb-randr | monitor hotplug & geometry | `libxcb-randr0-dev` | `libxcb` | `xcb-util-randr-devel` |
| xcb-xtest | XTEST | `libxcb-xtest0-dev` | `libxcb` | `xcb-util-xtest-devel` |
| xcb-shape | rounded corners | `libxcb-shape0-dev` | `libxcb` | `libxcb-devel` |
| fontconfig | font matching | `libfontconfig1-dev` | `fontconfig` | `fontconfig-devel` |
| freetype2 | text rasterization | `libfreetype6-dev` | `freetype2` | `freetype-devel` |
| pkg-config | build | `pkg-config` | `pkgconf` | `pkgconf-pkg-config` |
| imlib2 *(optional)* | wallpaper thumbnails | `libimlib2-dev` | `imlib2` | `imlib2-devel` |

Package names vary by release — search your distribution for the xcb-util
libraries if one is missing. Build with `AUSTERE_NO_IMLIB2=1` to omit the
Imlib2 dependency entirely. The TOML parser ([tomlc17](3rdparty/)) is
vendored and needs nothing extra.

### Build

```sh
make            # cc -std=c11 -Wall -Wextra -Werror -pedantic
```

Wallpaper thumbnails require Imlib2 (default on). To build without it:

```sh
make AUSTERE_NO_IMLIB2=1
```

### Install

```sh
sudo make install            # PREFIX=/usr/local by default
```

Installs the `austere` binary, the `austere-cmd` CLI, and
`contrib/austere.desktop` as a display-manager session, so `austere` shows
up in the login screen's session list. Override the prefix or use
`DESTDIR` for packaging:

```sh
make install PREFIX=/usr DESTDIR="$pkgdir"
```

To remove:

```sh
sudo make uninstall
```

### Start

Pick **austere** as the session in your display manager, or start it
manually:

```sh
exec austere
```

Swap it in over a running window manager without restarting X:

```sh
austere --replace
```

Other options: `--restart` (restart in place, restoring sessions).

## Features

- **Layouts**: tile (master/stack; global mode, per-ws ratio/nmaster),
  monocle, float
- **Workspaces** (i3 model, 9 per output set) across **multiple
  monitors** with RandR hotplug and workspace migration
- **Modular status bar**: built-ins (workspaces, layout, title, clock,
  battery, volume, cpu, ram) plus script modules over exec pipes — a
  script's every line is one frame; death freezes the last frame and
  fires an internal notification; configurable `bar_gap` floating inset
- **Settings**: TOML config (`~/.config/austere/austere.conf`) plus
  named config **states** (`super+grave`) that auto-apply when picked
  and persist across boots; transactional reload via state pick, the
  `reload` keybind (`super+Escape`), or SIGHUP — a malformed file never
  disturbs the running session; native settings menu (`super+e`) with
  live apply and `Ctrl+s` save
- **Panels**: window switcher (MRU quick-cycle + filtered list),
  launcher (PATH scan, prefix modules with `%s` templates, history),
  application menu (`super+d`, .desktop scan with category buckets,
  embedded logo icons, click-to-launch), welcome screen
- **Command socket**: `$XDG_RUNTIME_DIR/austere/socket` —
  `austere-cmd ws 3`, `austere-cmd reload`, `austere-cmd exec xterm`,
  `austere-cmd set_layout monocle`
- **EWMH subset**: pager atoms, urgency, fullscreen state,
  `_NET_CLOSE_WINDOW`; dock-type bar with workarea reservation
- **Extras**: restart-in-place with session restore, rounded corners
  (XShape, opt-in), window decorations (`deco = true`: title bar with
  close/maximize buttons), terminal swallowing (opt-in), first-boot
  autostart list (`[autostart] run`), wallpaper switcher (feh
  delegation; Imlib2 thumbnails, `AUSTERE_NO_IMLIB2=1` text fallback;
  RAM cache, flicker-free navigation)
- **Text**: any fontconfig font pattern (`font = "Agave Nerd Font Mono:
  pixelsize=16"`), full UTF-8 + extended glyph table, 4-bit coverage
  AA rendering with run merging, supersampled regeneration pipeline

## Usage

The first run writes `~/.config/austere/austere.conf` (fully commented)
and shows a welcome screen. Everything below is rebindable in
`[keys]` — these are the factory defaults.

### Windows

| Key | Action |
|---|---|
| `super+Return` | open a terminal |
| `alt+q` | close focused window |
| `super+f` | toggle fullscreen |
| `super+g` | toggle floating / tiling |
| `alt+← → ↑ ↓` | move focus (stacking layout) |
| `super+ctrl+m` | move focused window to the other monitor |
| `super+shift+s` | mark focused window as scratchpad |
| `super+p` | toggle scratchpad |

### Layouts

| Key | Action |
|---|---|
| `super+s` | cycle layout (tile → monocle → float) |
| `super+h` | shrink master area |
| `super+l` | grow master area |

### Panels

| Key | Action |
|---|---|
| `alt+Tab` / `alt+shift+Tab` | window switcher (forward / reverse) |
| `super+Tab` | quick-cycle windows by MRU |
| `alt+space` | launcher |
| `super+d` | application menu |
| `super+e` | settings menu |
| `super+grave` | config states picker |

### Workspaces

| Key | Action |
|---|---|
| `super+1 … super+9` | switch to workspace |
| `super+shift+1 … super+shift+9` | move focused window to workspace |

### System

| Key | Action |
|---|---|
| `super+m` | quit |
| `super+Escape` | reload configuration |
| `super+shift+r` | restart in place |
| `alt+r` | random wallpaper |
| `alt+w` | pick wallpaper |
| `XF86AudioRaise / Lower / Mute` | volume |

## Configuration

`~/.config/austere/austere.conf` is written on first run and is fully
commented. There is no file-watch — to apply a file you edit, reload it
(`super+Escape` or SIGHUP), or better, save it as a config **state**
under `~/.config/austere/states/` and pick it with `super+grave`. Highlights:

```toml
[appearance]
border_width = 2
corner_radius = 0        # > 0 rounds client corners via XShape
deco = true              # title bar with close/maximize on every window

[bar]
position = "top"         # top | bottom
bar_gap = 0              # floating inset from screen edge

[behavior]
focus_follows_mouse = true
swallowing = false       # terminals swallow their GUI children (§5.9)

[autostart]
run = ["picom", "dunst"]

[keys]
bind = ["super+Return spawn_terminal", "super+m quit"]

[[launcher.module]]
name = "web"
desc = "DuckDuckGo"
cmd = "xdg-open \"https://duckduckgo.com/?q=%s\""

[wallpaper]
setter_command = "feh --bg-scale %s"
dirs = ["~/Pictures/wallpapers"]
```

## Command socket

When `[general] socket = true` (the default), the running session exposes
a Unix socket at `$XDG_RUNTIME_DIR/austere/socket`. The bundled
`austere-cmd` sends any registered action over it:

```sh
austere-cmd ws 3                # switch to workspace 3
austere-cmd send_ws 2           # move focused window to workspace 2
austere-cmd set_layout monocle  # switch layout immediately
austere-cmd exec firefox        # launch a program
austere-cmd reload              # same as super+Escape
```

## Project layout

```text
src/              core WM (wm, events, layout, ewmh, socket, panels…)
src/layouts/      one file per layout
src/bar_modules/  one file per built-in status module
contrib/          austere-cmd CLI + display-manager session entry
3rdparty/         vendored tomlc17 (TOML parser)
docs/             SPEC.md, PHILOSOPHY.md
```

## Documentation

- [SPEC](docs/SPEC.md) — architecture specification, the source of truth
  for design decisions
- [PHILOSOPHY](docs/PHILOSOPHY.md) — why those decisions exist

## License

MIT — see [LICENSE](LICENSE).
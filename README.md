# austere

<p align="center"><img src="logo_white.svg" alt="austere" width="96"></p>

A low-spec X11 desktop shell: tiling/stacking/floating window manager,
modular status bar, app launcher, config switcher, window switcher and wallpaper picker.

this project is experimental software still being worked on. please forgive me for any goofy stuff :)

## app launcher
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/30574b6e-af89-48c6-a394-2fe2082d8286" />


## config/layout switcher (change your theme and layout on the fly with super+`)
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/7eca7184-c4b0-4813-a69f-70f715412129" />

## preview style window switcher similar to openbox/labwc
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/450e16e6-c2be-4769-b6dc-7d1c255ccc76" />

## wallpaper picker
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/a8103b4e-6052-43db-927c-11fbfa781e25" />

## themes (heres a couple. all major colorschemes to be added in the future or add them yourself! :) )
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/2493f7e8-18a7-422c-b2a5-77dc5c23f705" />
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/e9b3b432-ee75-49f2-b29c-40e95d26005f" />

## tiling ofc
<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/f92aecca-beef-4abc-b610-53efd74e49be" />



## Installation

Install the dependencies for your distribution, then build and install.
Three copy-paste blocks and you're done.

**Debian / Ubuntu**

```sh
sudo apt install -y git build-essential pkg-config libxcb1-dev libxcb-keysyms1-dev libxcb-icccm4-dev libxcb-randr0-dev libxcb-xtest0-dev libxcb-shape0-dev libfontconfig1-dev libfreetype6-dev libimlib2-dev
```

**Arch Linux**

```sh
sudo pacman -S --needed base-devel git libxcb xcb-util-keysyms xcb-util-wm fontconfig freetype2 imlib2
```

**Fedora**

```sh
sudo dnf install -y git gcc make pkgconf-pkg-config libxcb-devel xcb-util-keysyms-devel xcb-util-wm-devel xcb-util-randr-devel xcb-util-xtest-devel fontconfig-devel freetype-devel imlib2-devel
```

**Any distribution** — clone, build, install:

```sh
git clone https://github.com/jdanks420/austere
cd austere
make
sudo make install
```

*Package names vary by release — search your distribution for these if
one is missing. Remove with `sudo make uninstall`. Wallpaper thumbnails
need Imlib2 (default); build with `make AUSTERE_NO_IMLIB2=1` to skip it.
The TOML parser ([tomlc17](3rdparty/)) is vendored, so nothing else is
required.*

## Starting austere

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

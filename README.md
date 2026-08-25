# austere

A low-spec X11 desktop shell: tiling/stacking/floating window manager,
modular status bar, and runtime settings menu — in C11 against raw
[libxcb](https://cgit.freedesktop.org/xcb/libxcb). No toolkits, no
cairo, no font stack beyond core X bitmap fonts, one thread, one
connection, one `poll()` loop.

## Features

- **Tiling** (master/stack, per-workspace layout memory, ratio and
  nmaster binds) with monocle and floating layouts, edge snapping,
  Alt/super drag
- **Workspaces** (i3 model, 9 per output set) across **multiple
  monitors** with RandR hotplug and workspace migration
- **Modular status bar**: built-ins (workspaces, layout, title, clock,
  battery, volume) plus script modules over exec pipes — a script's
  every line is one frame; death freezes the last frame and fires an
  internal notification
- **Settings**: TOML config (`~/.config/austere/austere.conf`) with
  hot reload on save, SIGHUP, or keybind; transactional — a malformed
  file never disturbs the running session; native settings menu
  (`super+m`) with live apply and `Ctrl+s` save
- **Panels**: window switcher (MRU quick-cycle + filtered list),
  launcher (PATH scan, prefix modules with `%s` templates, history),
  welcome screen
- **Command socket**: `$XDG_RUNTIME_DIR/austere/socket` —
  `austere-cmd ws 3`, `austere-cmd reload`, `austere-cmd exec xterm`
- **EWMH subset**: pager atoms, urgency, fullscreen state,
  `_NET_CLOSE_WINDOW`; dock-type bar with workarea reservation
- **Extras**: restart-in-place with session restore, rounded corners
  (XShape, opt-in), terminal swallowing (opt-in), wallpaper switcher
  (feh delegation; Imlib2 thumbnails, `AUSTERE_NO_IMLIB2=1` text
  fallback)

## Building

```sh
make            # cc -std=c11 -Wall -Wextra -Werror -pedantic
make run        # launch inside Xephyr on a throwaway display
make check      # build + smoke suite + valgrind
make install    # PREFIX=/usr/local by default
```

Dependencies: libxcb, xcb-util (keysyms, icccm, randr, xtest), xcb-shape,
and [tomlc17](3rdparty/) (vendored). Optional: Imlib2 for wallpaper
thumbnails — build with `AUSTERE_NO_IMLIB2=1` to omit it entirely.

## Configuration

`~/.config/austere/austere.conf` is written on first run and is fully
commented. Save it to hot-reload. Highlights:

```toml
[appearance]
border_width = 2
corner_radius = 0        # > 0 rounds client corners via XShape

[bar]
position = "top"         # top | bottom

[behavior]
focus_follows_mouse = true
swallowing = false       # terminals swallow their GUI children (§5.9)

[keys]
bind = ["super+Return spawn_terminal", "super+shift+q quit"]

[[launcher.module]]
name = "web"
desc = "DuckDuckGo"
cmd = "xdg-open \"https://duckduckgo.com/?q=%s\""

[wallpaper]
setter_command = "feh --bg-scale %s"
dirs = ["~/Pictures/wallpapers"]
```

## Testing

`scripts/smoke.sh` is a capability-detecting suite that grows with the
WM; `make check` runs it under valgrind. Test-only helpers live in
`contrib/` (XTEST injectors, a scriptable test client, a monitor-topology
seam for Xephyr, a hostile-client torture tool).

## License

MIT — see the header of each source file.

# Austere — Architecture Specification

Austere is a low-spec, lightweight desktop shell for X11: a window manager
with built-in tiling, stacking, and floating layouts, plus a minimal statusbar.
It targets old hardware: small resident footprint, zero GPU/compositing work,
and only libxcb as a hard dependency.

This document is the source of truth for design decisions. `PHILOSOPHY.md`
(same directory) explains *why* those decisions exist — principles, the
deliberate divergences from suckless, and tiebreakers for gaps.

---

## 1. Goals and non-goals

### Goals

| Goal | Target (measured) |
|---|---|
| Resident memory | ≤ 4.5 MB PSS idle, < 3 MB private (measured 3.4 MB PSS / 3.1 MB private) |
| Binary size | ≤ 260 KB stripped (measured 252 KB, imlib2 build) |
| Dependencies | libxcb incl. `shape` (+ xcb-util keysyms/icccm/randr/xtest); `fontconfig` + `freetype` for text (no Xlib/Xft); `imlib2` only if built with wallpaper thumbnails (AUSTERE_NO_IMLIB2 omits it); `tomlc17` found in `3rdparty/` |
| Input latency | Direct event loop, no polling layers |
| Extensibility | New layouts added by writing one `.c` file + one registry line |
| Configurability | Every aspect adjustable at runtime — settings menu for normal use, named config *states* (pick to auto-apply + persist), plain-text config file + manual `reload` for advanced use |
| Bundled tools | Window switcher + app/script launcher built in — no dmenu/rofi/polybar processes |

### Non-goals (explicitly)

- Compositing, shadows, transparency, animations — these belong to the
  user's choice of external compositor, which austere fully supports but
  never requires (§5.8). Austere itself ships none.
- Wayland support (possible future fork, never mixed into this codebase).
- Embedded scripting engines (Lua/Fennel/etc.). The binary interprets no
  language: extension happens across process boundaries (user scripts as
  bar modules, §6.3) or compile boundaries (layouts, built-in modules).
  Configuration remains declarative data.
- Xinerama — RandR only.
- Session management (XSMP).

---

## 2. System overview

Single-process, single-threaded. One XCB connection, one event loop.

### Terminology (normative)

| Term | Meaning |
|---|---|
| window | A raw X11 window id |
| client | A window austere manages; owns a `Client` struct |
| workspace | Named container of clients; global array (§4.2), i3 model |
| monitor | A RandR output; displays exactly one workspace |
| layout | Algorithm assigning geometries to the workspaces' tiled clients; one global mode (§4.4) |
| arrange | One full geometry-recomputation pass over a monitor |
| action | Named operation in the central registry (§9.1); keys, mouse, menu, and socket are all frontends to it |
| panel | Transient WM-owned overlay window: settings menu / switcher / launcher |

The words *tag* and *desktop* do not appear in this codebase except inside
EWMH atom names.

```
                ┌─────────────────────────────────────────┐
   X server     │                 austere                  │
  ┌─────────┐   │  ┌────────┐   ┌────────────┐   ┌──────┐ │
  │ clients │◄──┼──┤ client ├──►│ layout     │◄──┤ keys │ │
  └─────────┘   │  │ mgmt   │   │ engine     │   └──────┘ │
       ▲        │  └────────┘   └────────────┘            │
       │        │       │            │                     │
  ┌─────────┐   │  ┌────────┐   ┌────────────┐   ┌──────┐ │
  │  RandR  │◄──┼──┤ monitor│   │ registry   │◄──┤mouse │ │
  │ outputs │   │  │ model  │   │ (layouts)  │   └──────┘ │
  └─────────┘   │  └────────┘   └────────────┘   ┌──────┐ │
       ▲        │  ┌────────┐                    │ ewmh │ │
       │        │  │  bar   │◄─── draw ──────────┤      │ │
  ┌─────────┐   │  └────────┘                    └──────┘ │
  │  bar    │◄──┼─────────────────────────────────────────┘
  └─────────┘   └─────────────────────────────────────────┘
```

Everything funnels through one `arrange()` cycle: any state change (client
added/removed/moved between workspaces, layout switch, workspace migration,
monitor geometry change) marks the affected monitor(s) dirty; the main loop
calls `arrange(monitor)` once per iteration for each dirty monitor, which
recomputes all client geometries via the active layout, then flushes.

### Event flow

One `poll()` over these fds: the X connection, the command-socket
listener (if enabled), per-connection socket fds, script-module stdout
pipes (§6.3), and a self-pipe for signal delivery (SIGHUP reload,
SIGINT/SIGTERM shutdown). Nothing else.

1. Drain `xcb_poll_for_event()` in a loop (no threads).
2. Dispatch through a switch on `event->response_type`.
3. Handlers mutate WM state and mark monitors/workspaces/bar dirty.
4. After the queue drains: arrange dirty monitors, redraw dirty bar, `flush`.

---

## 3. Module map

```
src/
  main.c         entry point, event loop, dispatch table
  wm.c/h         global WM state: client list, monitor list, focus
  client.c/h     client lifecycle, size hints, ICCCM property handling
  layout.c/h     layout registry + arrange driver
  layouts/       one file per layout (tile.c, float.c, monocle.c, ...)
  monitor.c/h    RandR output tracking, workspace↔monitor assignment
  bar.c/h        statusbar window, module registry, drawing, click routing
  bar_modules/   one file per built-in module (workspaces, clock, battery...)
  keys.c/h       keygrab setup, keycode/sym resolution, binding lookup
  mouse.c/h      button grabs, interactive move/resize
  ewmh.c/h       _NET_* atom support (subset)
  settings.c/h   Settings struct, compiled-in defaults, live-apply dispatcher
  conf.c/h       austere.conf parser/serializer (TOML via vendored tomlc17)
  states.c/h     config states: named .toml snippets, picker, boot override
  menu.c/h       shared native panel UI: list rendering, text input, key nav
  settings_menu.c/h  settings sections rendered over the panel API
  switcher.c/h   window switcher panel + MRU quick-cycle
  launcher.c/h   PATH scan cache, custom entries, command execution
  util.c/h       die(), clamp(), misc
```

Dependency rule: `layouts/*` may only see the public headers (`layout.h`,
`wm.h`, `settings.h`). Nothing includes another layout's file.

---

## 4. Core data structures

### 4.1 Client

```c
typedef enum { CLIENT_TILED, CLIENT_FLOATING, CLIENT_FULLSCREEN } ClientState;

typedef struct Client {
    xcb_window_t  win;
    unsigned int  ws;            /* owning workspace index (global array) */
    ClientState   state;
    bool          scratchpad;    /* designated dropdown window */
    int16_t       fx, fy;        /* floating geometry (preserved) */
    uint16_t      fw, fh;
    bool          never_focus;   /* docks, toolbars, hint-derived */
    xcb_window_t  transient_for; /* WM_TRANSIENT_FOR parent, XCB_NONE */
    SizeHints     hints;         /* min/max/inc/aspect from WM_NORMAL_HINTS */
    char         *title;         /* WM_NAME cache for the bar */
    struct Client *next, *prev;
} Client;
```

Clients live in one doubly-linked global list ordered by stacking/focus
recency. Tiling order = list order filtered by
`c->ws == monitor's visible workspace`.

### 4.2 Workspace

```c
#define WS_MAX 9

typedef struct Workspace {
    char          *name;         /* conf-provided or decimal fallback */
    Monitor       *mon;          /* output this workspace lives on */
    Client        *sel;          /* last-focused client, NULL if empty */
    float          split_ratio;  /* tile ratio, per-ws (tuning, not mode) */
    unsigned int   nmaster;
    bool           urgent;       /* some member raised urgency */
} Workspace;

extern Workspace workspaces[WS_MAX];   /* global array, defined in workspace.c */
```

The layout **mode** (tile / monocle / float) is global: one value applies
to every workspace, stored on `wm` (`layout_idx`). Switching the mode
mid-session changes all workspaces at once, so the bar glyph never lies
about what you are in. The tuning parameters (`split_ratio`, `nmaster`)
stay per-workspace, so each workspace keeps its own tile proportions.
Gaps are global (`Settings`) — deliberately not stored here.

Workspaces follow the **i3 model**, not dwm tagsets: each exists on exactly
one monitor at a time; every monitor shows exactly one workspace. Focusing
a foreign workspace moves focus to its owning monitor; an explicit bind
migrates a workspace to the focused monitor. On RandR hotplug, orphaned
workspaces re-home to surviving monitors.

### 4.3 Monitor

```c
typedef struct Monitor {
    Rect        geom;              /* output geometry from RandR */
    unsigned int ws_visible;       /* the single shown workspace */
    unsigned int ws_prev;          /* last visited, for toggle bind */
    Bar         bar;
    struct Monitor *next;
} Monitor;
```

### 4.4 Layout

The heart of extensibility. A layout is a value of:

```c
typedef struct Layout {
    const char *name;              /* registry key, e.g. "tile" */
    const char *symbol;            /* bar glyph, e.g. "[]=", "<>" */
    void (*arrange)(wm_t *, Monitor *m, Workspace *ws);
    bool floats_all;               /* true => arrange() leaves clients alone */
    bool ratio_aware;              /* true => uses ws->split_ratio (§5.4) */
} Layout;
```

Registered in one table in `layout.c`:

```c
const Layout layouts[] = {
    { "tile",    "[]=", arrange_tile,    false, true  },
    { "float",   "<>",  arrange_float,   true,  false },
    { "monocle", "[ ]", arrange_monocle, false, false },
    /* spiral, deck, grid, btree ... append here */
};
```

Contract every layout must obey (enforced by review checklist):

1. Read only: `m->geom` minus bar, `ws` parameters, and the client list
   filtered by `(c->ws == m->ws_visible)` and `state != CLIENT_FLOATING`.
2. Write only: client `x/y/w/h` via the shared helper `apply_geom(wm, c, ...)`
   which honors size hints and border width — never call `configure_window`
   directly.
3. Must handle 0 and 1 client lists correctly (no divide-by-zero, no
   negative widths).
4. Fullscreen clients are skipped by the driver before `arrange` runs.

Two distinct concepts, do not conflate: **`CLIENT_FLOATING` state** is a
per-client property (a window the user explicitly floated with
`toggle_float`, default `super+g`, while `tile` is active); **the `float`
layout** is a *global* mode that treats every client on every workspace
as floating (`floats_all = true`). In layout `tile`, individual floating
clients exist; in layout `float`, everything
is free-positioned and the tiling drivers never run. Clients keep their
`floating` flag across the switch, so a client floated in `tile` stays
floating when you return to `tile`.

Adding a layout = new file in `layouts/`, one function, one registry row,
optional `set_layout <name>` default in austere.conf. Nothing else changes.

### Layout roster

Implemented — registered in `src/layout.c`:

| Layout | Symbol | Behavior |
|---|---|---|
| `tile` | `[]=` | Master-stack; `nmaster` masters on the left, rest stacked right |
| `float` | `<>` | Free positioning; tiling driver ignores every client |
| `monocle` | `[ ]` | All maximized, stacked; focus cycles through |

Planned (priority order): `deck` (masters tiled, slaves all maximized on
top of each other), `grid` (even rows/columns, filled column-major),
`spiral`/`dwindle` (recursive alternating splits, bspwm-style), `btree`
(manual binary tree; user picks split direction per container),
`tabbed`/`stacked` (one visible client with a WM-drawn tab/title bar).

### Invariants (must hold at every observable point)

1. Every managed client's `ws` indexes a valid entry of `workspaces[]`.
2. Exactly one workspace per monitor is visible; every workspace lives on
   exactly one monitor.
3. The focused client — if any — is in some monitor's visible workspace.
4. After any state change, dirty monitors are arranged before the next
   flush; no intermediate inconsistent geometry is ever presented.
5. All geometry writes flow through `apply_geom()`.

---

## 5. X11 integration details

### 5.1 Selection & startup

- Acquire ownership of the `WM_S<n>` manager selection for the screen via
  `xcb_set_selection_owner`. Re-query with `xcb_get_selection_owner`: if
  another window owns it, print an error and exit — never fight a running
  WM. Announce EWMH support through `_NET_SUPPORTING_WM_CHECK`.
- Select SubstructureRedirect + SubstructureNotify on the root window.
- Scan for pre-existing mapped windows and adopt them (`scan()`); on
  restart-in-place, replay session state into the adopted set (§10).

### 5.2 Events handled

| Event | Response |
|---|---|
| `MAP_REQUEST` | Adopt as client, classify float/tile, insert, arrange |
| `UNMAP_NOTIFY` | If ignored-flag unset: unmanage |
| `DESTROY_NOTIFY` | Unmanage, fix focus (MRU neighbor), arrange |
| `CONFIGURE_REQUEST` | Floating: honor; tiled: refuse (re-apply tiling geom) |
| `ENTER_NOTIFY` | Focus-follows-mouse (config toggle) |
| `KEY_PRESS` | Binding lookup → action |
| `BUTTON_PRESS/RELEASE/MOTION` | Grabs: modifier+drag move/resize; bar clicks; panel input |
| `PROPERTY_NOTIFY` | WM_NAME/title updates, WM_NORMAL_HINTS, WM_HINTS (incl. urgency) |
| `EXPOSE` | Redraw bar / panel windows |
| `FOCUS_IN` | Track focus changes initiated by clients/pagers; update `_NET_ACTIVE_WINDOW` |
| `SELECTION_CLEAR` on WM_Sn | Another WM took over: release and exit cleanly |
| `CLIENT_MESSAGE` | EWMH: `_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`, `_NET_WM_STATE` fullscreen/attention |
| RandR events (`RRScreenChangeNotify`/`RRNotify`) | Re-enumerate outputs, re-home orphaned workspaces, re-assign clients |

### 5.3 Atoms required (minimum viable set)

ICCCM: `WM_PROTOCOLS`, `WM_DELETE_WINDOW`, `WM_STATE`, `WM_NORMAL_HINTS`,
`WM_HINTS`, `WM_NAME`, `WM_CLASS`, `WM_TRANSIENT_FOR`.
EWMH subset: `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`,
`_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`, `_NET_CURRENT_DESKTOP`,
`_NET_NUMBER_OF_DESKTOPS`, `_NET_DESKTOP_NAMES`, `_NET_WM_DESKTOP`,
`_NET_CLOSE_WINDOW`, `_NET_WM_PID`, `_NET_WM_STATE_FULLSCREEN`,
`_NET_WM_STATE_DEMANDS_ATTENTION`, `_NET_WM_WINDOW_TYPE` (dock/dialog/
toolbar classification only).
Motif hints: read `MOTIF_WM_HINTS` decorations flag (floats may request none).

Transient/dialog windows and any client with a minimum-size hint become
floating automatically. A transient stays stacked above its parent: if
its parent is raised or focused, the transient and its own transients
are re-raised above it (`raise_client()`). Windows whose
`_NET_WM_WINDOW_TYPE` is `dialog` or `toolbar` float even without a
`WM_TRANSIENT_FOR` hint.

### 5.4 Mouse interactions

- `<modifier>+Btn1` drag: move (floating clients only).
- `<modifier>+Btn3` drag: resize; which object depends on the target:
  - **Floating client** (or any client while the `float` layout is
    active): resizes the window freely (`apply_geom` with min/max-size
    hint clamping).
  - **Tiled client in a ratio-aware layout** (`tile`): adjusts the
    tiling boundary — horizontal drag changes the workspace's
    `split_ratio` (master ratio), clamped to `[0.1, 0.9]`, and persists
    in the workspace's layout parameters. `arrange()` gives synchronous
    feedback.
  - **Tiled client in a non-ratio layout** (`monocle`): no-op.
  `<modifier>` = `[mouse] modifier` setting, default `super`.
- **Tiling vs floating (i3 parity)**: no modifier-drag ever changes a
  window's floating state. Tiled windows cannot be drag-moved; float the
  window explicitly with `toggle_float` (default `super+g`), then move or
  resize freely.
- **Dragging a tiling boundary**: pressing Btn1 within a few pixels of a
  border between tiled clients grabs that split instead of the window;
  dragging adjusts it (master ratio, or the nearest internal split), and
  the new value persists in the workspace's layout parameters. Works in
  every ratio-aware layout; combined with `ratio_shrink`/`ratio_grow`
  binds this gives mouse *and* keyboard resizing of tiles. *(Handled via
  `super+Btn3` on a tiled client today; the edge-grab zone variant is
  future work.)*
- Pointer warps during grab; synthetic `ConfigureNotify` sent to the
  client after release (ICCCM §4.1.5 compliance).
- Click-to-focus always active; raise-on-click for floats (`raise_on_click`
  setting).

### 5.5 Scratchpad

One designated window, toggled with `super+p` (`scratch_toggle`):

- **Designation**: if no client is designated, austere spawns
  `[general] terminal` flagged to become the scratchpad (the flag attaches
  to the first client mapping from that spawn). The `scratch_mark` bind
  designates the focused client instead; running it again on the
  designated client releases the role.
- **Toggle**: maps/unmaps it centered on the focused monitor, always
  floating, always on top. While hidden it stays **managed but unmapped**:
  excluded from arrange and from switcher/MRU until shown again.
- **Workspace independence**: it displays over whatever is visible; its
  `ws` field keeps its original assignment so clearing the role returns it
  to normal behavior on that workspace.

### 5.6 Urgency

- Sources: ICCCM `WM_HINTS.urgency`, `_NET_WM_STATE_DEMANDS_ATTENTION`.
- Effect: client's workspace flagged urgent → its bar indicator renders in
  `urgent_color`; if the client is currently visible, its border does too.
- Cleared when that client gains focus (workspace flag recomputed); never
  self-clears on a timer.

### 5.7 Window shaping — rounded corners (opt-in)

- `corner_radius = 0` (default, disabled). Any value > 0 applies a rounded
  rectangular **bounding region** via the XShape extension to each managed
  client including its WM-drawn border.
- Applied inside `apply_geom()` so reshaping rides the normal arrange path;
  decorated windows round their **wrapper** too (`corner_radius > 0`), so the
  title strip's corners clip as one shape with the client; the clipped
  slivers reveal what is beneath (root background) since the wrapper keeps
  no background layer. Cleared while a client is fullscreen; never applied
  to bars or panels.
- Honest limitations, accepted: without a compositor there is no
  anti-aliasing and the clipped corner reveals whatever is beneath
  (typically the root background). This is period-correct hard-edged
  rounding, not macOS softness.

### 5.8 Compositor independence (contract)Austere runs identically with **any** compositor — picom (any version),
compton forks, others — or none at all. The contract that makes this hold:

1. Austere owns stacking, decoration, and layout only; shadows,
   transparency, fading, vsync are exclusively the compositor's domain.
2. No RGBA/ARGB visual requirements anywhere: every WM-drawn surface
   (bar, panels, borders) uses the root window's visual and opaque
   backgrounds.
3. Austere never claims the `_NET_WM_CM_Sn` selection — that belongs to
   the compositor — and ignores `_NET_WM_WINDOW_OPACITY` client messages
   gracefully if none handles them first.
4. XShape regions (§5.7) are the only decoration geometry a compositor
   must tolerate; both shaped and unshaped modes render correctly with or
   without one running.
5. Users launch their compositor of choice through `[autostart]` (or
   externally); austere makes no version or protocol assumptions beyond
   ICCCM/EWMH basics.

### 5.9 Window swallowing (opt-in)

Default **off** (`[behavior] swallowing = false`). When enabled: launching
a GUI app from a terminal replaces the terminal in its tile until the app
exits — the classic dwm-swallow workflow.

- **Matching**: new client's `_NET_WM_PID` ancestor chain (walk
  `/proc/<pid>/status` PPid links, depth-capped at 10) compared against
  managed clients' PIDs; nearest managed terminal-class ancestor is the
  victim. No match → normal tiling.
- **Mechanics**: the terminal becomes *managed-but-unmapped* (same
  machinery as the hidden scratchpad); the child takes its slot, geometry,
  and focus. On child exit the terminal remaps in place with its state
  intact.
- Exclusions: rules may set `swallow=false` per class (dialogs, browsers
  that spawn terminals, etc.). Swallowing never crosses workspace or
  monitor boundaries — no ancestor found there, no swallow.

---

## 6. Statusbar — modular architecture

The bar is an ordered sequence of **modules** drawn into three groups
(left, center, right) on a per-monitor xcb window. A registry unifies two
kinds of modules:

- **Built-in** — compiled C, same one-file-plus-one-row contract as
  layouts (`src/bar_modules/<name>.c`).
- **Script** — any executable the user points at; spawned once by austere,
  its stdout pipe is polled and each printed line becomes that module's
  text. The user's language choice is irrelevant to us.

Groups are user-defined name lists of built-ins (duplicates allowed);
script instances attach to the head monitor's right group via the
`AUSTERE_BAR_SCRIPTS` environment variable:

```ini
[bar]
modules_left   = ["workspaces", "layout", "title"]
modules_right  = ["volume", "clock", "battery"]
```

```sh
AUSTERE_BAR_SCRIPTS="mail:~/.config/austere/scripts/mail.sh" austere
```

### 6.1 Bar-level properties

`font`/`bg`/`fg`, `position` (`top`/`bottom`) and `bar_gap` are global
settings; height in `auto` mode = tallest active font + padding. Click
behavior stays owned by the module: workspaces/layout handle clicks,
others ignore them.

### 6.2 Built-in module roster

| Module | Source | Notes |
|---|---|---|
| `workspaces` | WM state | click to switch; urgent tint |
| `layout` | WM state | click cycles forward/backward |
| `title` | focused client | ellipsized to fit |
| `clock` | `strftime(time_format)` | minute timer via poll timeout |
| `battery` | `/sys/class/power_supply/*` | tokens `{cap}` `{state}`; renders empty when no battery exists |
| `volume` | shells `pactl`, falls back to `amixer`, at `interval` | deliberately impure: no uniform mixer ABI without libasound, which we refuse to link |
| `cpu` | `/proc/stat` delta | refresh on the 1 s stats tick; first frame shows 0% |
| `ram` | `/proc/meminfo` | `MemTotal`−`MemAvailable` as %; same 1 s tick |

Bar items fall into three placement groups — **left**, **center**, **right**
(§6.4). A missing group falls back to the default arrangement below;
place any subset in any order per monitor-wide via `[bar]` config
**list-type** keys — `modules_left`, `modules_center`, `modules_right` —
each an array of roaster names (unknown names abort a strict load). The
default arrangement encodes the group in the table order:
`workspaces`, `layout` on the left; `title` centered; `cpu`, `ram`,
`battery`, `volume`, `clock` on the right. Click hit-testing covers all
three groups; the settings menu stays reachable via right-click
anywhere on the bar.

### 6.3 Script-module protocol

- Spawned once at startup/reload with stdout → non-blocking pipe held in
  the poll set. **The script owns its cadence**: it sleeps/loops internally
  and prints one line whenever it wants new text rendered. Austere never
  polls scripts on a timer.
- One line = one frame. Lines are capped (4 KiB, truncated); output is
  UTF-8 rendered by freetype — non-ASCII depends on font coverage.
- **Death**: freeze last frame, warn on stderr, and surface an **internal
  notification popup** (§7.5): `module '<name>' died`. No auto-respawn (a
  crash-loop must not spin the CPU); `reload` or restart respawns it.
- **Reload semantics**: conf changes kill/respawn exactly the affected
  instances; untouched ones keep running so scripts can hold expensive
  state (e.g. IMAP connections).

### 6.4 Window & drawing mechanics

- One xcb window per monitor, docked top by default, height from §6.1.
  Sets `_NET_WM_WINDOW_TYPE_DOCK`; reserves space by manual geometry
  subtraction (not EWMH struts) so clients never overlap it.
- Text via client-side rasterization through fontconfig + freetype
  (pattern resolution offered by fontconfig; glyph coverage by freetype,
  composed into an ARGB line buffer and blitted with `draw_put_image24`).
  No Xlib, no Xft, no pango/cairo/toolkits. Glyph coverage and the
  composited line are cached per font (drawn on demand, idle frames
  allocate nothing). The color palette is allocated once from the root
  colormap and refreshed on settings reload.
- Right-click anywhere on the bar opens the settings menu (§9.3).
- Redraw policy: full redraw on any change; a bar is < 2000×20 px of
  image-text calls, trivially cheap on any hardware.

---

## 7. Overlay panels — switcher & launcher

Both tools are instances of one shared native panel component (`menu.c`):
a WM-owned window, centered on the focused monitor, keyboard-driven, drawn
with the exact same primitives as the bar (fontconfig/freetype text
compositing + rectangles).
Input is grabbed while a panel is open; all other X events queue normally
and are processed after close. Panels never appear in the client list.

### 7.1 Shared panel API

- Rows of plain text, optional prompt line, incremental substring filter
  over row labels as the user types.
- Nav: `Up/Down` or vim keys (wrap-around), `Tab` completes common prefix,
  `Enter` accepts, `Esc` cancels.
- A panel may preselect a row at open (`init_sel`); it is live-previewed
  immediately, so the switcher opens already advanced onto the next window.
- One allocation arena per open/close cycle; zero steady-state cost when no
  panel is open.
- The panel window is always kept above clients (re-raised after any live
  switch); releasing the opener's `Alt` closes an open panel (`hold_alt`).

### 7.2 Window switcher (`switcher.c`)

The WM keeps a global MRU list of clients — `focus()` promotes the focused
client to its head, `manage`/`unmanage` maintain it — so "most recently
used" is an intrinsic client property, independent of creation order.

Two modes:

1. **MRU quick-cycle** (`super+Tab`, default): each press steps focus through
    most-recently-focused order within the focused monitor's visible
    workspace. No UI, no grab state machine; a simple pointer walk of the
    MRU list.
2. **Panel mode** (`alt+Tab`, default): lists every managed client in MRU
    order with title · class · workspace · monitor marker (`*` = current),
    and opens with the **next** MRU window already selected and switched to
    (live preview on open). While open, `alt+Tab` moves the selection down,
    `alt+shift+Tab` moves it up (wrapping), and typing filters by
    title/class substring. The selected client is switched to live — its
    workspace and monitor shown, focus moved — as the selection changes, so
    moving the selection is itself the switch. Releasing `Alt` commits the
    selection and closes the panel; `Esc` cancels. Scope setting: all
    workspaces (default) vs. current monitor only.

### 7.3 Launcher (`launcher.c`) — dmenu × otter hybrid

A native panel (§7.1): type to filter, `Enter` executes. Combines dmenu's
instant app launching with otter-launcher-style **prefix modules** — but
unlike otter, everything renders natively; no terminal window, no window
rules, no TUI.

**Entry sources** (union, filtered as you type):

1. Executables found in `$PATH` (scanned at open, cached, invalidated by
   directory mtime) plus the configured `custom_dir`.
2. Static labeled commands: `entry = Label | command` lines in austere.conf.
3. **Prefix modules**: `[launcher.module.<name>]` sections — a description
   plus a command template where `%s` receives everything typed after the
   prefix.

```ini
[launcher.module.web]
desc    = DuckDuckGo search
cmd     = xdg-open "https://duckduckgo.com/?q=%s"

[launcher.module.sh]
desc    = Run in terminal
cmd     = xterm -e %s
```

**Routing on Enter:**

| Input shape | Resolution |
|---|---|
| `<prefix> <args>` matching a module | module `cmd`, `%s` = args |
| exact match of a labeled entry | that entry's command |
| name resolves in `$PATH` | direct `execvp`, no shell involved |
| other input + `default_module` set | whole input handed to that module |
| other input otherwise | error flash, prompt stays open |

While browsing, modules appear among the results; selecting one *inserts its
prefix into the prompt* instead of executing (args expected next). When the
current input starts with a known prefix, the module's description shows as
a hint right of the prompt line.

- **Execution**: templates run via `/bin/sh -c` through the double-fork
  spawn helper (never WM children); `%s` is substituted literally — quoting
  is the template author's job. Bare binaries skip the shell entirely.
- **History**: last N successful launches (modules included) sort to the
  top; persisted to `$XDG_DATA_HOME/austere/history` — deliberately *not*
  in austere.conf, so hand-edited config files are never churned by
  launcher usage.
- Switcher/launcher options and the module table are ordinary settings —
  subject to §9.4 completeness; the menu's Launcher section edits modules
  like its Keys section edits bindings.

### 7.4 Wallpaper switcher

Austere never draws the desktop wallpaper — it *chooses* a file and
delegates rendering to **feh** across the process boundary. The only image
work austere does at all is thumbnails for the picker, via Imlib2 (already
present on any system with feh; omitted entirely in
`AUSTERE_NO_IMLIB2=1` builds, which fall back to a text list).

```ini
[wallpaper]
dirs           = ~/Pictures/wallpapers ~/Pictures/photos   # scanned pool
recursive      = false
setter_command = feh --bg-scale %s       # %s = chosen path
thumb_w        = 192                     # grid cell content size
thumb_h        = 108
labels         = true                    # filename caption under thumb
cache_entries  = 48                      # LRU cap on decoded thumbs
```

- **Actions** (action registry → keybind / menu / socket):
  `wallpaper_random` (uniform pick from pool), `wallpaper_next` (sorted
  cycle), `wallpaper_set <path>`, `wallpaper_pick`.
- **Picker** (`alt+w`): a *full-screen* panel on the focused
  monitor showing the pool as a **grid of image thumbnails**, columns =
  floor(width / cell), filenames captioned when `labels = true`.

  | Input | Behavior |
  |---|---|
  | arrows / `h j k l` | move cell focus (wraps at row edges) |
  | `PgUp/PgDn`, mouse wheel | scroll by page |
  | `Home/End` | first/last cell |
  | single click | focus that cell |
  | **double click** (≤300 ms, same cell) | set wallpaper |
  | `Enter` | set focused wallpaper |
  | `Esc` | close |

- **Thumbnail pipeline**: decode lazily — visible cells first — scale to
  `thumb_w×thumb_h`, render into server-side pixmaps kept in an LRU cache
  (`cache_entries`) keyed by path+mtime. Process RSS stays flat; memory
  cost lands in the X server, bounded by the cache cap.
- **Set pipeline**: substitute `%s`, run via the double-fork spawn helper.
  feh owns `_XROOTPMAP_ID`/ESETROOT hints and all format decoding.
- Without Imlib2: same panel API renders a plain text list (§7.1); all
  keyboard actions identical minus imagery.

### 7.5 Internal notifications

Austere displays its own messages (module death, setter failures, reload
errors) without depending on any external notification daemon:

- One popup region per monitor: small bar-primitives window, top-right
  below the bar, opaque background.
- FIFO queue, depth 3 (oldest dropped); each entry auto-dismisses after
  `popup_timeout` seconds (default 5). Purely visual — no input grab,
  clicks pass through to whatever is beneath.
- Always mirrored to stderr; the popup is a convenience, not the record.

---

## 8. Command socket

Minimal external control surface for scripts (toggle: `[general] socket`).

- Path: `$XDG_RUNTIME_DIR/austere/socket` (fallback `/tmp/austere-$UID/`),
  created `0600` at startup, unlinked on clean exit.
- Protocol: `SOCK_STREAM`, UTF-8, one command per line:
  `action [arg...]` where *action* must exist in the action registry —
  the same table keybinds dispatch through. Reply per line: `ok` or
  `err <reason>`; server closes on client EOF.
- Examples: `ws 3`, `send_ws 3`, `layout next`, `exec xterm`, `reload`,
  `state dump`. Shipped helper: `contrib/austere-cmd`, a ~70-line C
  client sending one action per invocation.
- Hard rule: the socket adds zero WM logic — it is a third frontend of the
  action registry, exactly like keys and mouse.
- Failure isolation: socket I/O never blocks the X path; non-blocking
  accept, bounded reads, wedged clients are dropped.

---

## 9. Settings system

Two frontends, one backend. The `Settings` struct in `settings.c` is the
single source of truth at runtime; the settings menu and the config file are
both views over it.

```
                    ┌──────────────────┐
  austere.conf ──►  │  conf.c parser   │──┐
 (advanced users)   └──────────────────┘  │      ┌────────────────┐
                                          ├──►   │ Settings struct│ ► applied to WM
                    ┌──────────────────┐  │      │ + live-apply   │   state
     menu UI   ──►  │  menu.c editor   │──┘      └───────┬────────┘
 (normal users)     └──────────────────┘                 │ "Save"
                                                         ▼
                                                  conf.c serializer
                                                    writes austere.conf
```

### 9.1 Config file — `~/.config/austere/austere.conf`

Plain INI-like text: `section`, `key = value`, `#` comments. No expressions,
no variables, no shell-outs — parseable in one pass with zero dependencies.
Missing file or missing keys fall back to compiled-in defaults; the parser
never aborts the session on malformed input (§10).

```ini
[general]
terminal            = xterm -e     # used by spawn_terminal + scratchpad
socket              = true         # command socket (§8)
[appearance]
border_width        = 2
focus_color         = #5f819d
unfocus_color       = #444444
urgent_color        = #ff7f7f
gap                 = 6
smart_gaps          = true         # gaps vanish when one visible client
corner_radius       = 0            # >0 rounds clients AND decorations (off by default)
font                = "Agave Nerd Font Mono:pixelsize=16"

[deco]
deco                = false        # title bars on all windows
deco_title_h        = 20
deco_border         = #5f819d      # focused decoration border
deco_unfocus_border = #444444      # unfocused decoration border

[bar]
position            = top          # top | bottom
height              = auto         # auto | pixels
time_format         = %a %d %H:%M
bar_bg              = #1c1c1c
bar_fg              = #cccccc

[behavior]
focus_follows_mouse = true
raise_on_click      = true
snap_distance       = 12           # px edge snap for floats
swallowing          = false        # terminal swallow (§5.9), opt-in
popup_timeout       = 5            # seconds for internal notifications

[layouts]
default             = tile         # startup layout, one global mode for every workspace
nmaster             = 1
split_ratio         = 0.55
ratio_step          = 0.05

[workspaces]
names               = web dev term mail misc

[keys]
# bind = MODIFIER+MODIFIER+keysym ACTION [arg...]
bind = super+Return        spawn_terminal
bind = super+m             quit
bind = alt+q               close_focused
bind = super+s             cycle_layout
bind = super+g             toggle_float
bind = super+f             toggle_fullscreen
bind = super+h             ratio_shrink
bind = super+l             ratio_grow
bind = super+Tab           mru_step
bind = super+w             show_switcher
bind = alt+space           show_launcher
bind = super+d             menu_apps
bind = super+e             menu_settings
bind = super+grave         menu_states
bind = super+p             scratch_toggle
bind = super+shift+s       scratch_mark
bind = super+ctrl+m        ws_to_monitor
bind = super+Escape        reload
bind = super+shift+r       restart
bind = alt+r               wallpaper_random
bind = alt+w               wallpaper_pick
bind = alt+Left            focus_left
bind = alt+Right           focus_right
bind = alt+Up              focus_up
bind = alt+Down            focus_down

# laptop defaults: shell out to pactl (removable lines)
bind = XF86AudioRaiseVolume  volume_raise
bind = XF86AudioLowerVolume  volume_lower
bind = XF86AudioMute         volume_mute

[mouse]
modifier            = super        # drag modifier; Alt if unset
move_button         = 1
resize_button       = 3

[switcher]
scope               = all          # all | monitor

[launcher]
scan_path           = true
custom_dir          = ~/.local/bin # extra scripts beyond $PATH
history_size        = 20
default_module      = web          # unmatched input lands here; omit = refuse

[launcher.module.web]
desc                = DuckDuckGo search
cmd                 = xdg-open "https://duckduckgo.com/?q=%s"

[launcher.module.sh]
desc                = Run in terminal
cmd                 = xterm -e %s

[rules]
# class[:instance[:title]] → workspace float follow swallow never_focus
rule = Gimp:*                       ws=5 float=true
rule = *:xterm                      float=false swallow=false
rule = *:*:Preferences              float=true
rule = Slack:*                      ws=3 follow=true

[autostart]
exec = picom --experimental-backends   # example only; austere ships none
exec = feh --bg-scale ~/wall.jpg
```

Keybinds reference a fixed **action registry** (`settings.c`): a static table
mapping action names → function pointers with tagged-union args. New actions
are added by extending that table; the parser, menu, and command socket pick
them up without changes elsewhere.

Parser notes (all normative):

- Values are trimmed of whitespace; `#` starts a comment anywhere outside a
  quoted string. Leading `~` in paths expands to `$HOME` — no other
  expansion is performed.
- `[workspaces] names`: fewer than `WS_MAX` names → remaining workspaces use
  their decimal index as name; more names than `WS_MAX` → extras ignored
  with a warning.
- `[rules]` match `class[:instance[:title]]` with `*` globs; parameters
  `ws=<n>`, `float=<bool>`, `never_focus=<bool>`, `follow=<bool>`
  (switch view to the workspace when this window maps), `swallow=<bool>`
  (§5.9 opt-out) may be combined. Rules evaluate **only at manage time** —
  a reload never retroactively reclassifies already-mapped windows.
  Default behavior without `follow`: new windows on hidden workspaces only
  raise urgency — nothing ever steals focus.

### 9.2 Precedence, persistence & reload

1. Compiled-in defaults (lowest).
2. `austere.conf` values override defaults at startup / an explicit reload.
3. A picked **config state** (§"Config states" below) is the normal way to
   change configuration wholesale: picking one swaps in that file's
   settings *transactionally* and marks it as the boot default.
4. Menu edits mutate `Settings` live and take effect immediately.
5. **Save** in the menu serializes the full current `Settings` back to
   `austere.conf` (atomic write: temp file + rename), so GUI changes
   persist and the file never drifts from reality.

Known tradeoffs, accepted deliberately:

- Save rewrites `austere.conf` canonically — hand-written comments and
  ordering are not preserved. The file is machine-canonical after the first
  Save. States files, by contrast, are hand-authored and never rewritten by
  austere.

**Config states.** Complete `.toml` configs live in
`$XDG_CONFIG_HOME/austere/states/`. The state picker (`super+grave`) lists
them; picking one is the canonical reload path and **auto-applies
immediately** — the same transactional load as `reload` (§ below), then
writes the choice to `states/.last`. On boot, the marked state is loaded in
place of `austere.conf`. States are how config changes are made and
persisted in normal use.

**Reload** (transactional load of a file, shared by states, `reload`, and
SIGHUP). Two explicit triggers reach it directly:

1. **Keybind** — `reload` action (`super+Escape` by default).
2. **SIGHUP** — handler does nothing but `write(2)` to the self-pipe;
   no async-signal-unsafe work.

There is **no file-watch**: editing `austere.conf` or a state file on disk
never auto-triggers a reload (the inotify `hotwatch` mechanism was removed
in favor of explicit state picks — see §2, which no longer lists an inotify
fd). To apply a file you edit by hand, pick the state or press `reload`.

The load is **transactional**: parse + validate into a scratch `Settings`;
only if every value passes validation is it atomically swapped into place,
then applied as a diff — ungrab/regrab keys, re-render bar, re-arrange,
re-scan launcher cache. On any error the running config stays untouched and
a warning goes to stderr with file:line. Reload never touches session state:
open workspaces, focused clients, and floating geometry are preserved; only
configuration changes.

### 9.3 Settings menu

A native modal panel — same drawing primitives as the bar (fontconfig/
freetype text compositing + rectangles), no toolkit. Centered window ~60% of monitor width, one level
deep (categories as columns/sections on one screen, not nested dialogs).

- **Navigation**: arrows / vim keys, `Enter` selects/cycles value,
  `Left/Right` adjust sliders and enums, `Esc` closes, `Ctrl+s` saves.
- **Sections** mirror §9.1: General · Appearance · Bar · Behavior · Layouts ·
  Workspaces · Panels · Wallpaper · Keys · Rules · Autostart · Launcher.
- **Keys section**: lists current bindings; selecting a binding prompts
  "press new key combo" (raw keycode grab), then optionally an action picker.
- **Live apply**: every control dispatches through `settings_apply(key_id)`
  which updates WM state immediately — colors/gaps/bar move/redraw instantly;
  font change re-measures bar height and re-renders; keybind changes re-run
  `grab_keys()` after ungrabbing. Nothing ever requires a restart.
- Menu is just another mapped WM-owned window: input is grabbed while open,
  all other events queue normally, arrange() skips it like the bar.

### 9.4 What is configurable (completeness contract)

Every field of the `Settings` struct must be reachable from **both**
frontends. When adding a setting: add struct field → default → conf key →
menu row → live-apply case. A setting missing from either frontend is a
bug.

List-type settings (launcher modules/entries, wallpaper dirs, autostart
`run`, bar `modules_left/center/right`) are conf-only until the menu
grows list editors; their scalar siblings still carry menu rows. All of
them keep the live-apply case.

### 9.5 First run

1. If `austere.conf` does not exist at startup: write the fully-commented
   canonical default config to `~/.config/austere/`, then load it. The
   user's first edit surface exists before their first keystroke.
2. Once, per user (marker file `$XDG_DATA_HOME/austere/welcome`): show the
   **welcome panel** — one screen of essentials (`super+Return` terminal ·
   `alt+space` launcher · `alt+Tab` switcher · `super+e` settings ·
   `super+m` quit), dismissed by any key.
   Never shown again; marker created even if dismissed instantly.

---

## 10. Robustness rules

- **Never crash on malformed clients**: all property reads go through
  wrappers that validate lengths/types before use. A bad client must never
  take down the session.
- **Never crash on malformed config**: the parser validates every value
  (ranges, enums, hex colors, keysym names); invalid entries fall back to
  defaults with a warning on stderr, listing file:line.
- **Error handler**: install `xcb_set_event_handler` default handler that
  logs the opcode and continues. Only `die()` on connection death.
- **Leaks**: every malloc has exactly one owner; client structs freed on
  unmanage; titles freed on replace. Valgrind-clean shutdown path is a
  milestone gate.
- **Restart-safe**: `super+shift+r` serializes session state — each window's
  workspace, floating geometry, per-workspace tuning params, plus the global
  layout mode — to `$XDG_RUNTIME_DIR/austere/state`, then `execvp(self, argv)`. On startup,
  `scan()` adopts windows and replays matching entries; unmatched or stale
  entries are dropped and the file is consumed (deleted) either way.
- **Socket-safe**: a misbehaving socket client can stall or crash itself but
  never the WM (§8 failure isolation).

## 11. Performance notes (why this meets the low-spec goal)

- Zero allocations in steady state (event loop path allocates nothing except
  title strings on rename, and per-connection buffers on the socket path).
- One round-trip per frame of interaction worst case (`flush` + poll).
- Arrange cost is O(n) over visible clients with no sorting beyond the
  existing list order.
- Idle CPU ≈ 0%: blocked in `poll()`, woken only by real X events, the
  once-a-minute clock tick, or a config save.
- Panels (§7) allocate one arena per open/close; launcher PATH scan is
  cached and revalidated by mtime, so opening the launcher costs one
  directory stat in the common case.

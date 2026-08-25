# AGENTS.md — Austere

Instructions for AI agents (and humans) working on this repository.
**`docs/SPEC.md` is the source of truth for design.** If code and spec
disagree, fix the code — and if the *desired behavior* changed, propose a
spec edit first.

Austere is a low-spec X11 desktop shell: tiling/stacking/floating WM +
minimal statusbar + runtime settings menu, in C11 on libxcb only.

---

## Hard constraints (never violate)

1. **Dependencies**: libxcb (incl. `shape`) + xcb-util modules only
   (`keysyms`, `icccm`, `randr`, `image`, `event`). Never add
   cairo/pango/freetype/toolkits/threads. Sole exception: `imlib2` for
   wallpaper-picker thumbnails — omit entirely with
   `AUSTERE_NO_IMLIB2=1` (text-list picker fallback). Settings menu,
   switcher, launcher, and wallpaper picker use the same drawing
   primitives as the bar (thumbnails aside).
2. **Single thread**, single XCB connection, `poll()`-based event loop
   (X fd + inotify fd + command socket fds + script-module pipes +
   self-pipe, nothing else).
3. **Layout contract** (SPEC §4.4): layouts read state, write geometry only
   through `apply_geom()`, handle 0/1 clients, never call
   `xcb_configure_window` directly.
4. **Settings completeness contract** (SPEC §9.4): every new setting needs
   struct field + default + conf key + menu row + live-apply case. Missing
   any of the five = incomplete task.
5. No comments explaining *what* — code should say it. Comments only for
   non-obvious *why* (protocol quirks, ICCCM citations).

## Commands

```sh
make              # build (cc -std=c11 -Wall -Wextra -Werror -pedantic)
make run          # build + launch inside Xephyr on :1  (safe; never :0)
make check        # build + valgrind --leak-check=full smoke script
make clean
scripts/xephyr.sh start|stop|status   # the long-lived test display
scripts/smoke.sh                      # capability-detecting assertions
```

The Xephyr display (`:1`, 1280x800) is **long-lived**: start it once per
work session. `make run` rebuilds and restarts only austere inside it —
`--replace` takes over `WM_Sn` from the running instance without tearing
down clients, so the edit→test cycle is seconds. Test-only tools (never
linked into austere): xdotool (XTEST key injection), wmctrl/xprop (EWMH
readback), valgrind.

## Testing protocol

- **NEVER run austere against the host session / real display.** Only inside
  Xephyr via `make run` (`scripts/xephyr.sh`: starts `Xephyr :1 -screen
  1280x800`, then austere with DISPLAY=:1).
- Spawn test clients with `xterm`/`xclock` inside Xephyr. Verify:
  windows tile/float correctly, keybinds fire, bar updates, no crash on
  close via both WM_DELETE and xkill.
- Hot-reload check: while running in Xephyr, edit austere.conf and save —
  changes must apply without restart; a deliberately broken conf must leave
  the running session untouched.
- Xephyr input decays with session age: XTEST injections can silently
  stop delivering (to every client, not just austere). If keys stop
  landing, check `xev -root` during an injection — empty output means the
  display is wedged; `scripts/xephyr.sh stop && start` fixes it.
- xcb CreateWindow value lists MUST be ordered by mask-bit
  significance (e.g. BACK_PIXEL before OVERRIDE_REDIRECT before
  EVENT_MASK) — wrong order fails with BadValue asynchronously and the
  window silently never exists. This bit three separate windows (bar,
  popup, wallpaper picker) before being written down here.
- Imlib2's drawable rendering needs an Xlib display austere doesn't
  own — decode/scale with imlib, then blit via `xcb_put_image`
  (ARGB32 host-order data matches ZPixmap LSBFirst on x86).
- tomlc17's file parser returns an ok result for a MISSING file (empty
  document) — conf_load() must check access(path, R_OK) itself or a
  wrong XDG path silently discards every setting with zero warnings.
  Test setups mirroring $XDG_CONFIG_HOME must use the full
  `<dir>/austere/austere.conf` layout, not `<dir>/austere.conf`.
- A failed build leaves the previous binary in place; `make | grep -c
  error` pipelines can mask this and you end up testing stale code
  that "proves" your new code works. Check the build actually relinked
  before drawing conclusions from behavior.
- Injection scripts hard-refuse DISPLAY=:0/:1 (host displays): the
  persistent test shell often still exports `:0` from Xephyr restarts
  (Xephyr itself nests on the host display), and one missed `DISPLAY=`
  re-export once fired XTEST keybinds into the user's real session.
- Never `pkill` by generic name in test scripts — the user's own
  terminal/browser run on the host display and match too. Spawn test
  processes with a captured PID and kill that exact PID (a lost kitty
  + tmux window once died to `pkill -9 -f kitty`).
- Test tools that send events must round-trip before exiting
  (`xcb_get_input_focus_reply` after flush): `xcb_disconnect` right after
  `xcb_flush` can race the server reading the socket, silently dropping
  the request (bit monpoke and keyinject identically).
- `pkill -x austere` cannot see valgrind-wrapped instances (comm becomes
  `memcheck-*`) — kill by PID or `pkill -f memcheck`, or ghost instances
  keep WM_Sn and every later launch dies with "another WM already owns".
- `pkill -f PATTERN` also matches the *calling script's own cmdline*
  (the pattern is in its argv) — kills your own tool block mid-run.
  Use `-x`, or a bracket pattern (`pkill -f '[m]emcheck'`).
- xmalloc/calloc'd recycled heap chunks carry stale bytes and valgrind
  stays silent (calloc marks memory defined). Every struct field must be
  initialized at every construction site: `font_t.builtin` was left as
  garbage for server fonts and only crashed when a save+reload created
  the first one mid-session — two green gates had just gotten lucky
  with zeroed pages.
- Silent WM death mid-smoke = check `coredumpctl list | grep austere`
  first; `-g` in CFLAGS + `coredumpctl dump <pid> -o` + gdb gives the
  exact faulting line in under a minute. Hours of log-forensics lose to
  five minutes of core autopsy.
- Compositor matrix: at milestones touching appearance (M6+), smoke-test
  once without a compositor and once with picom inside Xephyr (if
  installed) — bar/panels/borders must render identically both ways.
- Every milestone gate: clean `-Werror` build, Xephyr smoke test passes,
  valgrind shows zero definitely-lost blocks on clean quit.

## Milestones

Build strictly in order. Each ends with a working, demonstrable system.
Detailed task breakdown and per-phase gates: **`docs/PLAN.md`**.

- [ ] **M0 Skeleton** — connect to X, take WM_Sn selection (exit politely if
      taken), root event mask, empty event loop, clean shutdown.
      *Done when:* runs in Xephyr, exits cleanly on quit bind.
- [ ] **M1 Client management** — manage/unmanage, ICCCM hints, transient→
      float classification, focus handling, WM_DELETE.
      *Done when:* xterm opens/closes/restarts without leaks.
- [ ] **M2 Float + mouse** — float layout, Alt/super+drag move & resize,
      snap-to-edge, click-to-focus.
- [ ] **M3 Layout engine** — registry + arrange driver, `tile` and `monocle`,
      per-workspace layout memory, ratio/nmaster binds.
      *Done when:* cycling layouts never leaves stale geometries.
- [x] **M4 Workspaces** — i3 model: global ws array, exactly one visible per
      monitor, ws↔monitor migration binds, move-client-to-ws, named
      workspaces from conf, scratchpad toggle, EWMH desktop atoms subset.
- [x] **M5 Multi-monitor** — RandR tracking, hotplug re-homing of orphaned
      workspaces, focus-follows foreign-workspace switching. Xephyr ignores
      `xrandr --setmonitor`, so topology is exercised through the
      `_AUSTERE_TEST_MONITORS` ClientMessage seam (`contrib/monpoke`,
      `scripts/multimon.sh`); production RandR path shares monitors_apply().
- [x] **M6 Statusbar & modules** — per-monitor bar, module registry with
      free ordering (built-ins: workspaces/layout/title/clock/battery/
      volume), script modules over exec pipes, per-module font/color,
      click actions, strut reservation, internal notification popups.
      Module ordering/colors are hardcoded defaults plus the
      `AUSTERE_BAR_SCRIPTS` dev seam until M7 delivers [bar] conf; the
      reload-respawns-only-affected gate lands with M7's hotwatch.
- [x] **M7 Settings core** — Settings struct, defaults, conf parser with
      validation + stderr warnings, serializer, action registry for keybinds,
      hotwatch (inotify) + debounced transactional reload via save/SIGHUP/
      keybind, first-run default-conf write-out. Rules/launcher/switcher/
      wallpaper/autostart sections arrive with their milestones (M9/M10);
      menu rows land with M8 per §9.4.
- [x] **M8 Settings menu** — native panel UI over all sections, live apply,
      save-to-file, keybind re-grab UI ("press new combo"). Single-file
      implementation in `src/menu.c` (PLAN's menu_keys.c folded in);
      rows > viewport scroll lands with M9's shared panel work.
      *Done when:* every setting changeable from menu without restart;
      save round-trips through file losslessly.
- [x] **M9 Panels** — shared panel UI (`menu.c`), window switcher (MRU
      quick-cycle + filtered panel across workspaces), launcher (PATH scan
      cache, custom_dir + conf entries, prefix modules with %s templates,
      history, exec), one-time welcome panel. Launcher module/entry
      editing is conf-only until the menu grows list editors; menu rows
      cover the scalar launcher/switcher settings (§9.4).
- [x] **M10 Polish** — EWMH subset completion, restart-in-place with session
      state save/restore, command socket + `contrib/austere-cmd`,
      rounded corners via XShape, wallpaper switcher (full-screen thumbnail
      grid picker, random/cycle/set actions, feh delegation, text fallback
      without imlib2), terminal swallowing (§5.9), `scan()` adoption of
      pre-existing windows, README, install target, embedded Agave Nerd
      bitmap font (`src/font_agave.h`, generated; conf `font` = pixel
      size digits 12-28, XLFD strings fall back to core fonts).
      MOTIF_WM_HINTS decoration hints and launcher module/entry menu
      editors remain open (both conf-only for now).

## Code conventions

- C11, snake_case, `t` suffix for types (`Client`, `Rect`). File-scope
  globals allowed **only** in `wm.c`; other modules receive pointers.
- Ownership: one malloc = one owner. Clients own their title string; wm owns
  clients; settings owns parsed strings. Free paths are explicit and
  valgrind-verified.
- All XCB replies: get → use → free, immediately. Helper wrappers for
  property reads that validate type/format before interpreting.
- New files go in `src/`, declared in matching `.h`, added to Makefile
  `SRC` list. Layouts live alone in `src/layouts/<name>.c`.
- Functions < ~80 lines; split rather than nest deeper than 3 levels.

## When something is ambiguous

Check SPEC.md first, then `docs/PHILOSOPHY.md` (principles + divergence-from-
suckless table + tiebreakers). If genuinely unspecified, prefer the option
that (a) removes code, (b) removes dependencies, (c) keeps the event loop
flat. Ask before: changing the data model (SPEC §4), adding a dependency,
or deviating from the layout/settings contracts.

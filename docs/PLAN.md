# Austere — Phased Build Plan

Scheduling document. `SPEC.md` stays the source of truth for *behavior*;
this file orders the work and defines per-phase gates. Milestones M0–M10
in AGENTS.md are the contract; each phase below expands one milestone into
tasks, files, and a done-gate. Sizes: **S** ≤ 2 days, **M** ≤ 4 days,
**L** open-ended (split if it grows past ~a week).

Standing rules applied to **every** phase:

- Clean build: `cc -std=c11 -Wall -Wextra -Werror -pedantic`.
- Smoke test in Xephyr (`make run`) — never against the host display.
- Valgrind: zero definitely-lost blocks on clean quit (`make check`).
- Any phase touching settings runs the §9.4 completeness checklist.
- Any phase touching appearance (M6+) runs the compositor matrix once
  (bare Xephyr, then with picom if installed).

---

## Phase 0 — Docs closeout & scaffold (S)

**Goal**: zero known-inconsistent documentation; an empty project that
builds.

| # | Task |
|---|---|
| 0.1 | Finish TOML conversion: rewrite SPEC §9.1 (TOML example, `[keys]` map, vendored-parser note); convert `[bar]`, wallpaper, launcher-module examples; replace remaining `austere.conf` → `austere.toml`; patch PHILOSOPHY ("INI lines" L27, `austere.conf` L36); patch AGENTS (hot-reload bullet, M7 wording, constraints note sanctioning `3rdparty/tomlc17.{c,h}` as vendored source, not a linked dep) |
| 0.2 | Scaffold: `src/`, `scripts/xephyr.sh`, `contrib/`, `3rdparty/`, Makefile (`SRC` list, `-Werror`, `run`/`check`/`clean` targets) |
| 0.3 | Pin tomlc17 version + record license hash in `3rdparty/LICENSE.tomlc17` |

**Gate**: `grep -ri 'ini\|austere\.conf' docs/` returns nothing spurious;
`make` succeeds on an empty `main()`.

---

## Phase M0 — Skeleton (S)

**Goal**: connect, own WM_Sn, empty event loop, clean shutdown.

| # | Task | Files |
|---|---|---|
| 0 | `xcb_connect`, pick screen, die politely with message if another WM owns `WM_Sn` | `wm.c` |
| 1 | Intern atom table in one batched cookie round-trip (§5.3 list) | `atoms.c` |
| 2 | Root event mask: `SUBSTRUCTURE_REDIRECT \| SUBSTRUCTURE_NOTIFY \| EVENT_STRUCTURE \| PROPERTY_CHANGE` | `wm.c` |
| 3 | `poll()` loop skeleton over the X fd + self-pipe (self-pipe exists now, SIGHUP wires into it in M7) | `event.c` |
| 4 | Hardcoded `super+shift+q` quit check via xcb-keysyms (generalized by the real registry in M7) | `wm.c` |
| 5 | `--replace` flag: if `WM_Sn` is owned, acquire anyway and wait for the old owner to exit — the fast dev loop restarts austere inside a live Xephyr while clients stay mapped (also the standard EWMH `--replace` users expect) | `wm.c` |
| 6 | Shutdown path: unmap nothing (we own no windows yet), release selection, flush, close, exit 0 | `wm.c` |

**Gate** *(AGENTS)*: runs in Xephyr, exits cleanly on the quit bind,
`--replace` takes over a second instance cleanly, valgrind-clean.

## Phase M1 — Client management (M)

**Goal**: manage/unmanage lifecycle, ICCCM hints, transient→float,
focus, WM_DELETE.

| # | Task | Files |
|---|---|---|
| 0 | Property-read helpers: validated type/format wrappers (normative §10) | `util.c` |
| 1 | `manage()`: read `WM_CLASS`/`WM_NAME`/hints/transient-for; float-classify transients + fixed-size hints; append to client list | `client.c` |
| 2 | Placeholder arrangement (maximize-on-monitor) so windows are usable before layouts exist | `wm.c` |
| 3 | Focus model: SetInputFocus + `_NET_ACTIVE_WINDOW`; `_NET_SUPPORTED`, `_NET_CLIENT_LIST`, `_NET_SUPPORTING_WM_CHECK` | `ewmh.c` |
| 4 | `WM_DELETE_WINDOW` protocol; unmanage on `DESTROY_NOTIFY`; synthetic-unmap counter on `UNMAP_NOTIFY` | `events.c` |

**Gate**: xterm opens/closes/restarts without leaks; xkill path clean;
valgrind gate.

## Phase M2 — Float + mouse (M)

**Goal**: drag move/resize, snapping, click-to-focus.

| # | Task | Files |
|---|---|---|
| 0 | Button grabs on managed windows + root | `mouse.c` |
| 1 | Drag engine: modifier+Btn1 move, Btn3 resize, pointer warp during grab, auto-promote-to-float, min-size-hint respect | `mouse.c` |
| 2 | Snap-to-edge within `snap_distance`; `raise_on_click`; border rendering with focus/unfocus/urgent colors | `mouse.c`, `draw.c` |
| 3 | Synthetic `ConfigureNotify` post-release (ICCCM §4.1.5) | `mouse.c` |

**Gate**: drags feel right at 1280×800 in Xephyr; borders never stale;
0 leaks across 50 drag cycles.

## Phase M3 — Layout engine (M–L)

**Goal**: registry + driver, `tile` & `monocle`, per-workspace memory,
ratio binds, tiling-border drag.

| # | Task | Files |
|---|---|---|
| 0 | Layout contract header: `{name, arrange(ws, monitor)}`; registry table; `arrange()` driver iterating visible clients | `layout.h`, `wm.c` |
| 1 | `apply_geom()` choke point — the *only* caller of `xcb_configure_window` for tiled clients (contract §4.4) | `wm.c` |
| 2 | `layouts/tile.c`: nmaster stack, split_ratio, 0/1-client cases | `layouts/tile.c` |
| 3 | `layouts/monocle.c`: full-area stack, raise-focused | `layouts/monocle.c` |
| 4 | Float formalized as a registered layout (was ad-hoc in M2) | `layouts/float.c` |
| 5 | Per-workspace params (`nmaster`, `split_ratio`) + binds: `cycle_layout`, `set_layout`, `ratio_shrink/grow`, `nmaster_inc/dec` | `actions.c` |
| 6 | Tiling-boundary drag: grab zone near borders adjusts nearest split, persists in workspace params (§5.4) | `mouse.c` |

**Gate** *(AGENTS)*: cycling layouts never leaves stale geometries;
audit greps prove no direct configure calls outside `apply_geom`.

## Phase M4 — Workspaces (M)

**Goal**: i3-model global array, migration, scratchpad, urgency,
EWMH desktops, named-from-conf.

| # | Task | Files |
|---|---|---|
| 0 | **Vendor tomlc17 now** (pulled forward from M7 to honor "named workspaces from conf"); minimal `conf.c` API reading only `[workspaces].names` — generalized in M7 | `conf.c`, `3rdparty/` |
| 1 | `Workspace[WS_MAX]` global: name, monitor ptr, layout id, params, MRU list; `Monitor.ws_visible`/`ws_prev` (invariants §4.5) | `workspace.c` |
| 2 | Binds: `view_ws n`, `send_ws n`, toggle-back (`view_ws` on current → `ws_prev`) | `actions.c` |
| 3 | Scratchpad: mark/toggle via managed-but-unmapped machinery (reused by swallowing in M10) | `scratchpad.c` |
| 4 | Urgency: hints polling on PropertyNotify, `_NET_WM_STATE_DEMANDS_ATTENTION`, urgent border color | `ewmh.c` |
| 5 | EWMH desktop subset: `_NET_NUMBER_OF_DESKTOPS`, `_NET_DESKTOP_NAMES`, `_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP` | `ewmh.c` |

**Gate**: pagers see 9 correct desktops; migration binds never orphan a
workspace or double-show one (invariant asserts in debug builds).

## Phase M5 — Multi-monitor (M)

**Goal**: RandR tracking, hotplug re-homing, focus-follows-switching.

| # | Task | Files |
|---|---|---|
| 0 | Monitor enumeration via xcb-randr CRTC geometry | `monitor.c` |
| 1 | Hotplug events: orphaned workspaces re-homed to nearest monitor by center; `ws_visible` invariant restored | `monitor.c` |
| 2 | Focus follows foreign-workspace switching (viewing a ws homed elsewhere moves input) | `workspace.c` |
| 3 | Test rig: `xrandr` inside Xephyr with multi-output configs | `scripts/` |

**Gate**: plug/unplug cycles in Xephyr leave exactly one visible ws per
monitor and no lost clients.

## Phase M6 — Statusbar & modules (L)

**Goal**: modular per-monitor bar, built-ins, script modules, popups.

| # | Task | Files |
|---|---|---|
| 0 | Text/rect primitives with font-extent measurement — foundation for all panels later | `draw.c` |
| 1 | Bar window per monitor, strut reservation (`_NET_WORKAREA`), top/bottom + auto-height | `bar.c` |
| 2 | Module registry: `{name, instance, render, click}`, free ordering, duplicates allowed; defaults hardcoded until M7 | `bar.c` |
| 3 | Built-ins: workspaces (click-to-view), layout, title (ellipsize), clock (strftime, tick aligned to minute via poll timeout), battery (sysfs, 30 s refresh), volume (pactl→amixer shell-out; impurity documented) | `bar_modules/*.c` |
| 4 | Script modules: double-fork spawn, non-blocking stdout pipe in poll set, line-per-update protocol, per-instance font/color | `module.c` |
| 5 | Death handling: freeze last frame, stderr warn, internal popup (§7.5); no auto-respawn; conf reload respawns affected only | `module.c` |
| 6 | Notification popups: FIFO depth 3, `popup_timeout`, pass-through input, stderr mirror | `popup.c` |
| 7 | Click actions route through the action registry | `bar.c` |

**Gate** *(AGENTS)*: live user-script updates; kill → frozen frame +
popup; reload respawns only affected instances; compositor matrix run.

## Phase M7 — Settings core (L)

**Goal**: full TOML settings, validation, hot reload, first-run write-out.

| # | Task | Files |
|---|---|---|
| 0 | Complete `Settings` struct + defaults table (single source of truth) | `settings.c` |
| 1 | `conf.c` generalization: whole-file parse via tomlc17 → validate every field (ranges/enums/colors/keysym names) → warnings with file:line → scratch-settings API for transactional reload | `conf.c` |
| 2 | Canonical TOML serializer, atomic write (temp + rename) | `conf.c` |
| 3 | Action registry finalized: static `{name, fn, tagged-union arg}` table; keys, mouse, socket, menu all dispatch through it | `actions.c` |
| 4 | Keybind parser: `super+ctrl+alt+shift+keysym` grammar; `grab_keys()`/ungrab re-run | `keys.c` |
| 5 | `hotwatch.c`: inotify on austere.toml, `IN_CLOSE_WRITE\|IN_MOVED_TO`, ~100 ms debounce, self-pipe wake | `hotwatch.c` |
| 6 | Reload transaction: parse-scratch → validate → swap → diff-apply (regrab, bar re-render, re-arrange, launcher rescan); any error aborts leaving session untouched; failure surfaces as internal popup + stderr | `settings.c` |
| 7 | First-run: missing file → write fully-commented canonical default (§9.5) | `conf.c` |
| 8 | SIGHUP → self-pipe (handler writes one byte, nothing else) | `event.c` |

**Gate** *(AGENTS)*: save applies instantly; deliberately broken TOML
leaves session untouched; fresh boot produces written commented conf;
completeness checklist passes for every field.

## Phase M8 — Settings menu (M–L)

**Goal**: native panel UI over all sections, live apply, save, re-grab UI.

| # | Task | Files |
|---|---|---|
| 0 | Panel framework: centered ~60 % width modal on `draw.c` primitives, input grabbed while open, skipped by `arrange()` | `menu.c` |
| 1 | One-screen section columns; nav arrows/vim, Enter cycles, Left/Right adjusts, Esc closes, Ctrl+s saves | `menu.c` |
| 2 | Row generators for every §9.1 table — generated from the defaults table so the completeness contract is structural, not aspirational | `menu.c` |
| 3 | Live apply via `settings_apply(key_id)` — nothing requires restart | `settings.c` |
| 4 | Keys section: binding list, "press new combo" raw-keycode grab, optional action picker | `menu_keys.c` |
| 5 | Save → serializer → round-trip verification | `menu.c` |

**Gate** *(AGENTS)*: every setting changeable from menu; save round-trips
losslessly; no leaks across 100 open/close cycles.

## Phase M9 — Panels (L)

**Goal**: switcher, launcher, welcome — all on the M8 framework.

| # | Task | Files |
|---|---|---|
| 0 | Shared list/filter primitives extracted from menu experience | `menu.c` |
| 1 | Switcher: MRU quick-cycle (existing `mru_step`) + filtered panel across workspaces per `scope`; selection focuses client and views its workspace | `switcher.c` |
| 2 | Launcher: PATH scan cache mtime-revalidated, `custom_dir` merge, history load/save, prompt + hint line, prefix modules with `%s` templates, routing table (prefix > label > execvp > default > flash) | `launcher.c` |
| 3 | Execution: `/bin/sh -c` double-fork for templates, direct execvp for bare binaries | `util.c` |
| 4 | Welcome panel one-shot with `$XDG_DATA_HOME/austere/welcome` marker (§9.5) | `welcome.c` |

**Gate** *(AGENTS)*: partial-name launch works; prefix module routes args;
cross-workspace switcher focus switches views; no leaks on repeated opens.

## Phase M10 — Polish (L)

**Goal**: finish EWMH, restart, socket, corners, wallpaper, swallowing.

| # | Task |
|---|---|
| 0 | EWMH subset completion (`_NET_WORKAREA` per monitor, `_NET_WM_NAME`…) |
| 1 | Restart-in-place: session serialize → `execvp(self)` → `scan()` adoption replay; state consumed either way |
| 2 | Command socket (§8) + `contrib/austere-cmd` helper |
| 3 | Rounded corners via XShape when `corner_radius > 0` |
| 4 | Wallpaper switcher: thumbnail grid picker, feh delegation, `AUSTERE_NO_IMLIB2=1` text fallback |
| 5 | Terminal swallowing (§5.9): PID ancestry walk, managed-but-unmapped reuse, rule opt-outs |
| 6 | Full `scan()` adoption of pre-existing windows |
| 7 | Hostile-client hardening: `contrib/hostile-client.c` (malformed hints/classes/names, rapid map-unmap loops) + socket garbage-line fuzz once §8 exists |
| 8 | README, `make install` target |

**Gate**: entire AGENTS testing protocol green end-to-end; budgets met
(<2 MB RSS, ≈0 % idle CPU); valgrind-clean quit after exercising every
feature including restart and swallow cycles.

---

## Dependency spine (why this order)

```
draw.c ──► bar ──► popups ──► menu/panels
   │                    ▲
   └── M2 borders       │
action-registry-lite(M0) ═► layouts(M3) ═► workspaces(M4) ═► monitors(M5)
                            conf-lite(M4) ═► settings(M7) ═► menu(M8) ═► panels(M9)
```

Each phase unlocks the next's testability: layouts need clients (M1),
workspaces need layouts to remember (M3), monitors need workspaces to
re-home (M4), the bar needs monitors to sit on (M5), the menu needs the
registry to edit (M7), panels reuse the menu (M8). Nothing is built twice;
nothing waits on something unbuilt.

## Risk register

| Risk | Mitigation |
|---|---|
| tomlc17 quality/license | Pin version + hash in Phase 0; MIT; wrapper isolates it behind `conf.c` so swapping parsers touches one file |
| Bitmap fonts can't render UTF-8 titles | Accepted limitation (Latin-1 via `image_text_8`); documented in README; no freetype exception will be granted |
| Volume/brightness actions shell out | Documented impurity, same class as feh delegation; removable conf lines |
| Multi-monitor testing fidelity | Xephyr + xrandr emulates outputs well enough; real-hardware smoke deferred to install day |
| picom absent in test env | Compositor matrix degrades to bare-X run with a note; not a gate blocker |
| Test-tool deps (xdotool, wmctrl, xprop, valgrind) | Dev-machine only, never linked into austere; documented in README prerequisites-for-hacking section |

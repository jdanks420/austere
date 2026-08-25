# Austere — Design Philosophy

Normative. When SPEC.md is silent, this document decides. Order of
authority: SPEC.md → this document → AGENTS.md tiebreakers.

---

## Thesis

Austere applies **suckless engineering discipline to a product suckless
would refuse to build**: a desktop shell that is minimal under the hood and
accessible on the surface. New users configure it from a menu; advanced
users edit a plain-text file; nobody ever recompiles to change behavior.

## The problem being solved

suckless software (dwm et al.) achieves minimalism honestly — small code,
few dependencies, no runtime parsing — but gates *all* customization behind
edit-source-and-recompile. That gate is the deliberate cost of their purity,
and it excludes exactly the people a desktop should serve:

- newcomers, who should not need to learn C to change a font;
- tinkerers mid-session, for whom a rebuild-and-restart cycle destroys
  context and flow.

Austere's bet: the discipline and the agility are separable. Parse-time
cost lives at startup and reload (~microseconds for a few hundred INI
lines); steady-state cost stays zero. Purity of the *binary* matters more
than purity of the *workflow*.

## Principles (in priority order)

1. **Zero-rebuild configurability.** Every user-tunable behavior is runtime
   data, applied live. Recompiling is for developers changing behavior, not
   users changing preferences.
2. **Two frontends, one backend.** The settings menu and `austere.conf` are
   equally complete views over the same `Settings` struct (SPEC §9.4).
   Neither is second-class; a setting missing from either frontend is a bug.
3. **Dependencies are radioactive.** libxcb family only — with two named,
   justified exceptions: `xcb-shape` (rounded corners) and `imlib2`
   (wallpaper thumbnails only; already required by the mandated feh setter,
   build-omittable via `AUSTERE_NO_IMLIB2`). A feature needing anything
   else gets redesigned or dropped.
4. **Flat beats clever.** One thread, one `poll()` loop, no abstraction
   layers that exist "for later."
5. **Small enough to hold in your head.** Per-module size discipline;
   layouts are self-contained single files; deleting any one file must
   suggest where its logic lived.
6. **Progressive disclosure.** Defaults work bare; the menu covers daily
   tuning; the conf file covers depth (rules, keybind scripts); C covers
   what data cannot (new layouts) — via a one-file-plus-one-row contract.
7. **Configuration is data; extension has exactly two gates.** The binary
   parses no language and embeds no interpreter. Behavior extends across
   process boundaries (user scripts as bar modules) or compile boundaries
   (layouts, built-in C modules) — nothing in between.
8. **Performance budgets are features.** <2 MB RSS, ~0% idle CPU. A
   regression against budget is a bug, not a tradeoff.
9. **Robust by default.** Malformed config, hostile clients, dead sockets —
   nothing takes down the running session (SPEC §10).

## Where we diverge from suckless — explicitly

| suckless position | austere position | reason |
|---|---|---|
| Compile-time `config.h` | Runtime conf + hot reload + menu | The recompile gate blocks new users and mid-session iteration |
| No GUI config ("learn C") | Native settings panel drawn with bar primitives | Accessibility without toolkits or deps |
| Tags (multi-tag bitmasks) | i3-model workspaces | Matches the mental model most users already have |
| No remote control on principle | Command socket over the action registry | Scriptability costs zero WM logic paths |
| Launcher/menu = external process (dmenu) | Built-in panels | Fewer processes and zero RAM overhead on old hardware; same primitives |
| Status = root window name only | Modular bar: built-ins + process-boundary user scripts (root-name mode kept for dwm compat) | Full customization without embedding an interpreter |
| Compositing: out of scope, unaddressed | Explicit host-agnostic contract (SPEC §5.8): any user compositor or none | Users keep free choice of compositor/version; austere never fights it |
| Xlib | libxcb | Lower latency, smaller footprint |

Inherited without apology: single binary, bitmap fonts, low SLOC targets,
keyboard-first operation, flat event loop, patch-friendly structure.

## Tiebreakers (when SPEC and principles don't decide)

1. Prefer the option that removes code.
2. Then the one that removes dependencies.
3. Then the one keeping the event loop flat.
4. If options tie: choose the one serving **both** user classes; when forced
   to favor one, favor the newcomer path unless it costs performance or
   dependencies.
5. Still stuck: stop and ask.

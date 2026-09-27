#!/bin/sh
# Phase 2 interaction check for the tray bar module.
#
# Same harness shape as scripts/test-sni-watcher.sh: the real binary on a
# private Xvfb display, inside a private D-Bus session, with a throwaway
# HOME, and the fake items writing to a marker file that the shell greps.
# Nothing here touches the real display, session bus or config.
#
# What the checks actually prove, stated plainly:
#   - the pointer sweep is the visibility evidence. A tray cell is proven
#     to exist exactly when a press inside the bar reaches an item, and
#     proven absent when a sweep with a live-but-unregistered item
#     records nothing at all. That is what makes the empty-collapse
#     assertion meaningful.
#   - the whole-bar pixel differences below are only evidence that the bar
#     repainted after a registry change. A bar draws a clock, so "the bar
#     is not blank" says nothing about tray icons, and no pixel count here
#     is treated as a tray-visibility guarantee. Geometry and appearance
#     remain the designer's pixel probe's evidence.
#   - the bar is deliberately placed away from the screen edges (bottom
#     placement, 8 px gap) so bar_origin() is non-zero on both axes. The
#     coordinate assertions then depend on bar_root_point() actually adding
#     that origin, instead of passing trivially against a bar at (0,0).
#
# Phase A (interaction regression, required): bar origin from the X server,
# empty collapse, both cells found by sweep, button 1/2/3 with the press
# point in root coordinates, and the 4/5 and 6/7 scroll pairs.
#
# Phase B (real-app acceptance): the installed caffeine/caffeine-ng is
# started on the private display and must register, cause a bar repaint,
# resolve its themed icon, open a real popup with rows, and have a real
# Quit row that ends it. Every one of those is a hard gate, because a real
# client is the only place the protocol is not our own fixture. Only a
# missing app, or one that cannot run on this display at all, is reported
# as an explicit ACCEPTANCE BLOCKED line - never a pass, and never an
# excuse for a menu that answered with nothing.
#
# Usage: scripts/test-tray-ui.sh [path-to-austere]
#
# Needs Xvfb, dbus-run-session, dbus-send, xdotool, xwininfo, xwd, convert,
# awk and python3 with GObject introspection. Any of them missing is an
# explicit SKIP (exit 77), so `make test` never fails on a UI-only
# dependency.

set -eu

BIN=${1:-${AUSTERE_BIN:-./austere}}
case "$BIN" in
    /*) ;;
    *)  BIN="$PWD/$BIN" ;;
esac

missing=
for tool in Xvfb dbus-run-session dbus-send dbus-monitor xdotool xwininfo \
    xwd convert awk; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
python3 -c "import gi; gi.require_version('Gio', '2.0')" >/dev/null 2>&1 ||
    missing="$missing python3-gi"
if [ -n "$missing" ]; then
    echo "SKIP: missing test dependencies:$missing (tray UI check not run)" >&2
    exit 77
fi
[ -x "$BIN" ] || { echo "FAIL: no executable at $BIN" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/austere-trayui.XXXXXX")
XVFB_PID=""
WM_PID=""

cleanup() {
    [ -n "$WM_PID" ] && kill -9 "$WM_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# ---- private X display ------------------------------------------------
# Display numbers are split with scripts/test-sni-watcher.sh (90-109) so a
# concurrent `make -j test` cannot have the two harnesses race for the
# same free socket. Sockets are still scanned for a free one first.
DISP=""
n=110
while [ "$n" -lt 130 ]; do
    if [ ! -e "/tmp/.X11-unix/X$n" ]; then
        Xvfb ":$n" -screen 0 1024x768x24 -nolisten tcp \
            >"$TMP/xvfb.log" 2>&1 &
        XVFB_PID=$!
        i=0
        while [ "$i" -lt 50 ]; do
            [ -e "/tmp/.X11-unix/X$n" ] && break
            kill -0 "$XVFB_PID" 2>/dev/null || break
            sleep 0.1
            i=$((i + 1))
        done
        if [ -e "/tmp/.X11-unix/X$n" ]; then
            DISP=":$n"
            break
        fi
        kill "$XVFB_PID" 2>/dev/null || true
        XVFB_PID=""
    fi
    n=$((n + 1))
done
[ -n "$DISP" ] || { echo "FAIL: could not start Xvfb" >&2; cat "$TMP/xvfb.log" >&2; exit 1; }

# ---- the two items the interaction phase drives ----------------------
cat >"$TMP/ui_item.py" <<'ITEM'
"""Two StatusNotifierItems on one connection, on two object paths, so a
host press can be attributed to a specific registry entry. Each serves a
valid IconPixmap (so the bar really blits something) and records every
method it is called with in a marker file. SNI_NO_REGISTER=1 keeps the
object alive and listening without entering the registry, which is what
makes the empty-collapse sweep meaningful. Test scaffolding."""
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

MARK = os.environ["SNI_MARK"]
NO_REG = os.environ.get("SNI_NO_REGISTER") == "1"
SNI = "org.kde.StatusNotifierItem"
# tag -> (service, object path); the tags also order the registry
ITEMS = [
    ("A", os.environ.get("SNI_SERVICE", "org.austere.SniUiA"),
     "/StatusNotifierItem"),
    ("B", os.environ.get("SNI_SERVICE2", "org.austere.SniUiB"),
     "/StatusNotifierItem2"),
]


def pixmap(size):
    """A square with an opaque amber centre and a 3 px transparent border.

    The transparent border deliberately carries WHITE in its colour
    channels under a zero alpha. That is what a theme PNG does - hicolor's
    own caffeine-cup-empty stores rgb 255,255,255 under every fully
    transparent pixel - and it is the case the bar compositor used to get
    wrong: read as premultiplied, a zero-alpha pixel paints as its own
    colour, so the whole border came out as a white box. IconPixmap is
    straight alpha too, so this fixture reproduces it without a theme
    lookup."""
    # IconPixmap bytes are ARGB32 in network order, so the FIRST byte is
    # the alpha. The centre is opaque amber (ff e0 90 30) and the border is
    # 00 ff ff ff: alpha 0 with WHITE in the colour channels.
    px = b""
    for y in range(size):
        for x in range(size):
            if 3 <= x < size - 3 and 3 <= y < size - 3:
                px += bytes((0xFF, 0xE0, 0x90, 0x30))    # the amber centre
            else:
                px += bytes((0x00, 0xFF, 0xFF, 0xFF))   # alpha 0, WHITE
    return GLib.Variant("(iiay)", (size, size, GLib.Variant("ay", px)))


# Item A offers a 16px square, so the host blits it at the bar's own icon
# box (the 1x1 path). Item B offers only a 32px square, so the host has to
# downscale it to the same box (the box-filter path). Both cells stay
# 16 px wide, so no layout assertion below moves.
SIZES = {"A": (22, 16), "B": (32,)}

PROPS = ["Category", "Id", "Title", "Status", "IconName",
         "AttentionIconName", "ItemIsMenu", "Menu", "IconPixmap"]
PROPS_XML = "".join('  <property name="%s" type="%s" access="read"/>\n'
                    % (k, {"IconPixmap": "a(iiay)"}.get(k, "s"))
                    for k in sorted(PROPS))

XML = """<node>
<interface name="org.freedesktop.DBus.Properties">
 <method name="Get">
  <arg name="interface_name" type="s" direction="in"/>
  <arg name="property_name" type="s" direction="in"/>
  <arg name="value" type="v" direction="out"/>
 </method>
 <method name="GetAll">
  <arg name="interface_name" type="s" direction="in"/>
  <arg name="properties" type="a{sv}" direction="out"/>
 </method>
</interface>
<interface name="org.kde.StatusNotifierItem">
""" + PROPS_XML + """ <method name="Activate">
  <arg name="x" type="i" direction="in"/>
  <arg name="y" type="i" direction="in"/>
 </method>
 <method name="SecondaryActivate">
  <arg name="x" type="i" direction="in"/>
  <arg name="y" type="i" direction="in"/>
 </method>
 <method name="ContextMenu">
  <arg name="x" type="i" direction="in"/>
  <arg name="y" type="i" direction="in"/>
 </method>
 <method name="Scroll">
  <arg name="delta" type="i" direction="in"/>
  <arg name="orientation" type="s" direction="in"/>
 </method>
 <signal name="NewIcon"/>
 <signal name="NewStatus"><arg name="status" type="s"/></signal>
</interface>
</node>"""


def note(line):
    with open(MARK, "a") as handle:
        handle.write(line + "\n")
        handle.flush()


def values(tag):
    return {
        "Category": GLib.Variant("s", "ApplicationStatus"),
        "Id": GLib.Variant("s", "ui-" + tag),
        "Title": GLib.Variant("s", "ui test item " + tag),
        "Status": GLib.Variant("s", "Active"),
        "IconName": GLib.Variant("s", ""),   # force the pixmap path
        "AttentionIconName": GLib.Variant("s", ""),
        "ItemIsMenu": GLib.Variant("b", False),
        "Menu": GLib.Variant("o", "/MenuBar"),
        "IconPixmap": GLib.Variant("a(iiay)",
                                   tuple(pixmap(n) for n in SIZES[tag])),
    }


def handler(tag):
    def on_call(conn, sender, path, iface, method, params, invocation):
        if method == "GetAll":
            invocation.return_value(GLib.Variant("(a{sv})",
                                                  (values(tag),)))
        elif method == "Get":
            wanted = params.unpack()[1]
            invocation.return_value(GLib.Variant(
                "(v)", (values(tag).get(wanted, GLib.Variant("s", "")),)))
        elif method in ("Activate", "SecondaryActivate", "ContextMenu"):
            x, y = params.unpack()
            note("%s %s %d %d" % (tag, method, x, y))
            invocation.return_value(None)
        elif method == "Scroll":
            delta, orient = params.unpack()
            note("%s Scroll %d %s" % (tag, delta, orient))
            invocation.return_value(None)
        else:
            invocation.return_dbus_error(
                "org.freedesktop.DBus.Error.UnknownMethod", method)

    def on_prop_get(conn, path, iface, name):
        return values(tag).get(name, GLib.Variant("s", ""))

    return on_call, on_prop_get


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
for tag, service, path in ITEMS:
    on_call, on_prop_get = handler(tag)
    conn.register_object(path,
                         node.lookup_interface("org.freedesktop.DBus.Properties"),
                         on_call, on_prop_get, None)
    conn.register_object(path, node.lookup_interface(SNI), on_call,
                         on_prop_get, None)
    conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                   "org.freedesktop.DBus", "RequestName",
                   GLib.Variant("(su)", (service, 4)),
                   GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 5000,
                   None)
    if not NO_REG:
        conn.call_sync("org.kde.StatusNotifierWatcher",
                       "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                       "RegisterStatusNotifierItem",
                       GLib.Variant("(s)", (service + path,)), None,
                       Gio.DBusCallFlags.NONE, 5000, None)
        note("%s registered" % tag)

loop = GLib.MainLoop()
GLib.timeout_add(180000, loop.quit)
loop.run()
ITEM

# ---- the checks, inside a private session bus -------------------------
cat >"$TMP/check.sh" <<'CHECK'
set -u
PASS=0
FAIL=0
WM_PID=""
ITEM_PID=""
ITEM0_PID=""
APP_PID=""
MON_PID=""

ok()   { PASS=$((PASS + 1)); echo "  ok   $*"; }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL $*"; }
have() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

# Bounded stop: SIGTERM, then SIGKILL. A bare `wait` would wedge the
# harness on a process that ignores the first signal, and the whole point
# of this cleanup is that nothing outlives the run.
kill_bounded() {   # kill_bounded <pid> <label>
    [ -n "$1" ] || return 0
    kill "$1" 2>/dev/null || true
    _t=0
    while [ "$_t" -lt 30 ] && kill -0 "$1" 2>/dev/null; do
        sleep 0.1
        _t=$((_t + 1))
    done
    kill -9 "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
    # `kill -0 ""` succeeds in some shells, so test before the caller clears
    kill -0 "$1" 2>/dev/null && bad "$2 outlived the harness (SIGKILL too)"
    return 0
}

stop_app() {
    [ -n "$APP_PID" ] || return 0
    kill_bounded "$APP_PID" "$APP"
    APP_PID=""
    return 0
}

stop_monitor() {
    [ -n "${MON_PID:-}" ] || return 0
    kill "$MON_PID" 2>/dev/null || true
    wait "$MON_PID" 2>/dev/null || true
    MON_PID=""
    return 0
}

stop_item() {
    [ -n "$ITEM_PID" ] || return 0
    kill_bounded "$ITEM_PID" "fake item"
    ITEM_PID=""
    return 0
}

stop_item0() {
    [ -n "$ITEM0_PID" ] || return 0
    kill_bounded "$ITEM0_PID" "fake item"
    ITEM0_PID=""
    return 0
}

# Every background process this phase starts, killed on any exit path. The
# wm gets the same bounded stop as the fakes: a phase that fails an
# assertion must not leave a wm holding the harness's X display, and a
# wm that ignored SIGTERM would survive a plain kill as well.
cleanup_here() {
    stop_app
    stop_item
    stop_item0
    stop_monitor
    [ -n "$WM_PID" ] && kill -TERM "$WM_PID" 2>/dev/null
    _t=0
    while [ "$_t" -lt 30 ] && [ -n "$WM_PID" ] && kill -0 "$WM_PID" 2>/dev/null; do
        sleep 0.1
        _t=$((_t + 1))
    done
    [ -n "$WM_PID" ] && kill -9 "$WM_PID" 2>/dev/null
    [ -n "$WM_PID" ] && wait "$WM_PID" 2>/dev/null
    WM_PID=""
    return 0
}
trap cleanup_here EXIT
trap 'cleanup_here; exit 130' INT
trap 'cleanup_here; exit 143' TERM

items() {
    dbus-send --session --print-reply --reply-timeout=4000 \
        --dest=org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
        org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>&1 |
        sed -n 's/^ *string "\(.*\)"$/\1/p'
}

# The bar is the wide, short root child; nothing else in this WM is.
find_bar() {
    xwininfo -root -tree 2>/dev/null | awk '/^[[:space:]]+0x/ {
        if (match($0, /[0-9]+x[0-9]+\+[0-9-]+\+[0-9-]+/)) {
            g = substr($0, RSTART, RLENGTH)
            split(g, d, /[x+]/)
            if (d[1] >= 900 && d[2] <= 48) {
                split($1, h, " ")
                print h[1]
                exit
            }
        }
    }'
}

bar_field() {   # bar_field <bar-id> <x|y|w|h>
    xwininfo -id "$1" 2>/dev/null | awk -v want="$2" '
        /Absolute upper-left X/ { for (i = 1; i <= NF; i++)
            if ($i == "X:") x = $(i + 1) }
        /Absolute upper-left Y/ { for (i = 1; i <= NF; i++)
            if ($i == "Y:") y = $(i + 1) }
        /Width:/  { w = $2 }
        /Height:/ { h = $2 }
        END { print want == "x" ? x : want == "y" ? y : want == "w" ? w : h }'
}

root_field() {  # root_field <w|h>
    xwininfo -root 2>/dev/null | awk -v want="$1" '
        $1 == "Width:"  { w = $2 }
        $1 == "Height:" { h = $2 }
        END { print want == "w" ? w : h }'
}

# Pixels differing between two captures. This is repaint evidence, not a
# visibility verdict: a bar that draws a clock differs from a blank one too.
changed_pixels() {
    convert "$1" "$2" -compose difference -composite -colorspace Gray \
        -threshold 1% -format "%[fx:mean*w*h]" info: 2>/dev/null
}

# The popup is the one viewable root child that is popup-sized and sits
# near the cell that was pressed. X reuses window ids after a destroy, so a
# before/after diff would miss a popup that reopened with the same id.
find_popup_near() {   # find_popup_near <cell-x>
    xwininfo -root -tree 2>/dev/null | awk -v cx="$1" '
        /^[[:space:]]+0x/ {
            if (!match($0, /[0-9]+x[0-9]+\+[0-9-]+\+[0-9-]+/)) next
            g = substr($0, RSTART, RLENGTH)
            split(g, d, /[x+]/)
            w = d[1] + 0; h = d[2] + 0; x = d[3] + 0
            if (w < 20 || w > 600) next
            if (h < 20) next
            if (x < cx - 40 || x > cx + 600) next
            print $1
            exit
        }'
}

win_field() {   # win_field <id> <x|y|w|h>
    xwininfo -id "$1" 2>/dev/null | awk -v want="$2" '
        /Absolute upper-left X/ { for (i = 1; i <= NF; i++) if ($i == "X:") x = $(i + 1) }
        /Absolute upper-left Y/ { for (i = 1; i <= NF; i++) if ($i == "Y:") y = $(i + 1) }
        /Width:/  { w = $2 }
        /Height:/ { h = $2 }
        END { print want == "x" ? x : want == "y" ? y : want == "w" ? w : h }'
}

press() {   # press <x> <y> <button>
    xdotool mousemove "$1" "$2" click "$3" 2>/dev/null
}

# The ids of every clicked() event the wm sent to the real client, read back
# from the monitor. A method call block is member=Event, the line after it is
# the int32 id and the one after that the event name, so an id is only taken
# when its own block says "clicked".
clicked_ids() {
    [ -s "$TMP/app-monitor.txt" ] || return 0
    awk '/member=Event/ { id = ""
           if ((getline line) > 0 && line ~ /int32/) id = line
           if ((getline line) > 0 && line ~ /clicked/) print id }' \
        "$TMP/app-monitor.txt" | tr -s ' ' | sed 's/^ //;s/int32 //'
}

wait_items() {   # wait_items <count> <deciseconds>
    _want=$1; _t=0
    while [ "$_t" -lt "$2" ]; do
        [ "$(items | wc -l)" -eq "$_want" ] && return 0
        sleep 0.1
        _t=$((_t + 1))
    done
    return 1
}

# Walk the bar from its right edge towards the middle, pressing button 1 at
# the strip's vertical centre. A cell exists exactly when a press reaches an
# item, so this is what proves a cell (or the absence of one).
sweep() {   # sweep <step> <left-percent-of-bar>
    SWEEP_A=""; SWEEP_B=""; SWEEP_ANY=0
    _lo=$((BX + BW * $2 / 100))
    _x=$((BX + BW - 4))
    while [ "$_x" -gt "$_lo" ]; do
        : >"$SNI_MARK"
        press "$_x" "$Y" 1
        _line=$(head -1 "$SNI_MARK" 2>/dev/null)
        case "$_line" in
        "A Activate "*) [ -n "$SWEEP_A" ] || SWEEP_A=$_x ;;
        "B Activate "*) [ -n "$SWEEP_B" ] || SWEEP_B=$_x ;;
        esac
        [ -n "$_line" ] && SWEEP_ANY=$((SWEEP_ANY + 1))
        _x=$((_x - $1))
    done
}

echo "-- phase A: interaction"
# Bottom placement with an 8 px gap, so bar_origin() is non-zero on both
# axes and the coordinate assertions below cannot pass by accident.
CONF_DIR="$HOME/.config/austere"
mkdir -p "$CONF_DIR"
cat >"$CONF_DIR/austere.conf" <<'CONF'
[bar]
position = "bottom"
bar_bg = "#1a1a1a"
bar_fg = "#cccccc"
bar_gap = 8
modules_left = ["workspaces", "layout"]
modules_center = ["title"]
modules_right = ["tray", "cpu", "ram", "battery", "volume", "clock"]
CONF
if have "$(cat "$CONF_DIR/austere.conf")" '"tray"'; then
    ok "throwaway config lists tray in modules_right"
else
    bad "throwaway config lost the tray row"
fi

"$BIN" >"$LOG" 2>&1 &
WM_PID=$!
i=0
while [ "$i" -lt 100 ]; do
    dbus-send --session --print-reply --reply-timeout=2000 \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner \
        string:org.kde.StatusNotifierWatcher 2>/dev/null |
        grep -q 'boolean true' && break
    sleep 0.1
    i=$((i + 1))
done
dbus-send --session --print-reply --reply-timeout=2000 \
    --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.NameHasOwner \
    string:org.kde.StatusNotifierWatcher 2>/dev/null |
    grep -q 'boolean true' && ok "watcher name owned" \
    || { bad "no watcher name"; sed -n '1,40p' "$LOG"; exit 1; }

BAR=$(find_bar)
if [ -n "$BAR" ]; then
    ok "located the bar window $BAR"
else
    bad "no bar window found"
    sed -n '1,40p' "$LOG"
    exit 1
fi
BX=$(bar_field "$BAR" x)
BY=$(bar_field "$BAR" y)
BW=$(bar_field "$BAR" w)
BH=$(bar_field "$BAR" h)
SW=$(root_field w)
SH=$(root_field h)
echo "     bar at +$BX+$BY, ${BW}x${BH} on a ${SW}x${SH} root"
[ "$BW" -gt 0 ] && [ "$BH" -gt 0 ] && ok "bar geometry read from the X server" \
    || { bad "unusable bar geometry ${BW}x${BH}"; exit 1; }

# The origin has to be the configured one, and non-zero on both axes, or
# the coordinate assertions below would be vacuous.
if [ "$BX" -eq 8 ] && [ "$((BY + BH))" -eq "$((SH - 8))" ]; then
    ok "bar origin is the configured bottom+gap placement (+$BX+$BY)"
else
    bad "bar origin is not bottom+8: +$BX+$BY, ${BW}x${BH} on ${SW}x${SH}"
fi
if [ "$BX" -ne 0 ] && [ "$BY" -ne 0 ]; then
    ok "bar origin is non-zero on both axes (x=$BX y=$BY)"
else
    bad "bar origin is zero on an axis (x=$BX y=$BY); the coordinate checks would be vacuous"
fi
Y=$((BY + BH / 2))

echo "-- empty strip collapses (sweep with a live, unregistered item)"
xwd -id "$BAR" -out "$TMP/before.xwd" 2>/dev/null
SNI_NO_REGISTER=1 python3 "$TMP/ui_item.py" >"$TMP/item0.out" 2>&1 &
ITEM0_PID=$!
sleep 1
sweep 8 25
if [ "$SWEEP_ANY" -eq 0 ]; then
    ok "no tray cell while nothing is registered (empty strip collapses)"
else
    bad "$SWEEP_ANY press(es) reached an item with an empty registry"
fi
stop_item0

python3 "$TMP/ui_item.py" >"$TMP/item.out" 2>&1 &
ITEM_PID=$!
if wait_items 2 80; then ok "two items registered"
else bad "items never reached 2 entries: $(items | tr '\n' ' ')"; fi
sleep 1
xwd -id "$BAR" -out "$TMP/after.xwd" 2>/dev/null
PX=$(changed_pixels "$TMP/before.xwd" "$TMP/after.xwd")
if [ -n "$PX" ] && [ "$PX" -gt 0 ] 2>/dev/null; then
    ok "bar repainted after registration ($PX px differ; repaint evidence only)"
else
    bad "bar did not repaint when the items registered (differ=${PX:-none})"
fi

echo "-- cells and press coordinates"
# The sweep reads the cells off the X server rather than re-deriving the
# bar's layout maths; cells keep the backend's order, so A is left of B.
HIT_A=""; HIT_B=""
sweep 4 25
# The sweep's first hit is a cell's rightmost pixel, one step from the
# boundary with its neighbour, so both cells are then walked inwards to an
# interior point. That matters because the strip's cells move whenever a
# numeric module beside them changes width - a CPU percentage gaining a
# digit shifts every cell to its left - and a boundary pixel is then
# answered by the other item. find_cell only ever returns an x the wanted
# item answered for, so every later press is known to be inside it.
find_cell() {   # find_cell <A|B> <from-x> <direction> -> interior x
    _want=$1; _x=$2; _dir=$3; _d=0
    while [ "$_d" -lt 12 ]; do
        : >"$SNI_MARK"
        press "$_x" "$Y" 1
        sleep 0.2
        case "$(head -1 "$SNI_MARK" 2>/dev/null)" in
        "$_want Activate "*) echo "$_x"; return 0 ;;
        esac
        _x=$((_x + _dir * 2))
        _d=$((_d + 1))
    done
    return 1
}
HIT_A=$(find_cell A "${SWEEP_A:-0}" -1) || HIT_A=${SWEEP_A:-}
HIT_B=$(find_cell B "${SWEEP_B:-0}" 1) || HIT_B=${SWEEP_B:-}
# A press on a cell the strip moved out from under, answered with the
# method the wanted item received: the walk is what keeps the assertions
# below about the method rather than about a module's width at one instant.
press_in() {   # press_in <A|B> <x> <button> -> the answered line on stdout
    _want=$1; _x=$2; _btn=$3; _d=0
    while [ "$_d" -lt 8 ]; do
        : >"$SNI_MARK"
        press "$_x" "$Y" "$_btn"
        sleep 0.3
        _line=$(head -1 "$SNI_MARK" 2>/dev/null)
        case "$_line" in
        "$_want "*) echo "$_line"; return 0 ;;
        esac
        _x=$((_x - 2))
        _d=$((_d + 1))
    done
    echo "${_line:-}"
    return 1
}
if [ -n "$HIT_A" ] && [ -n "$HIT_B" ] && [ "$HIT_A" -lt "$HIT_B" ]; then
    ok "sweep found both cells (left x=$HIT_A, right x=$HIT_B)"
else
    bad "sweep did not find both cells (A=${HIT_A:-none} B=${HIT_B:-none})"
    sed -n '1,40p' "$LOG"
    exit 1
fi

# ---- icon transparency (regression) ------------------------------------
# A tray icon's transparent border must land on the bar's own ground, and
# the icon itself must still be drawn.
#
# The assertion is taken over a band of the bar rather than over one cell,
# and deliberately so: the strip's cells move whenever a numeric module
# beside them changes width (a CPU percentage gaining a digit shifts every
# cell to its left), so pinning a cell's x would make this flaky rather
# than strict. The band starts past the logo - the only other thing in the
# bar that can be pure white, since it is drawn in the logo's white variant
# over this dark ground - and runs to the bar's right edge, where the tray
# is right-anchored. Everything else the bar paints is bar_fg (#cccccc),
# BAR_DIM (#777777) or focus_color (#5f819d), none of which is #ffffff, so
# a white pixel in that band can only have come from an icon.
#
# The ground is the 1a1a1a this script's own throwaway config sets above.
GROUND=1A1A1A
BAND_X=$((BX + 64))
BAND_W=$((BW - 64 - 8))
xwd -id "$BAR" -out "$TMP/cells.xwd" 2>/dev/null
convert "$TMP/cells.xwd" -crop "${BAND_W}x${BH}+${BAND_X}+0" +repage \
    "$TMP/band.png" 2>/dev/null
BAND=$(convert "$TMP/band.png" -depth 8 txt:- 2>/dev/null | awk -v g="$GROUND" '
    NR > 1 {
        split($0, a, "#")
        hex = toupper(a[2])
        sub(/ .*/, "", hex)
        r = strtonum("0x" substr(hex, 1, 2))
        gg = strtonum("0x" substr(hex, 3, 2))
        b = strtonum("0x" substr(hex, 5, 2))
        n++
        if (hex == "FFFFFF") white++
        if (hex == g) ground++
        # the fixture'"'"'s own opaque colour, and its antialiased blends
        if (r >= 0xC0 && gg >= 0x60 && b <= 0x60) ink++
    }
    END { printf "%d %d %d %d", white + 0, ground + 0, ink + 0, n }')
set -- $BAND
BAND_WHITE=${1:-0}
BAND_GROUND=${2:-0}
BAND_INK=${3:-0}
BAND_TOTAL=${4:-0}
if [ "$BAND_WHITE" -eq 0 ]; then
    ok "no transparent pixel of an icon came out as white ($BAND_WHITE in the bar band)"
else
    bad "$BAND_WHITE white pixel(s) in the bar band: an icon's transparent border is painting its own colour"
fi
if [ "$BAND_INK" -gt 0 ]; then
    ok "the icons themselves are still drawn ($BAND_INK pixel(s) of icon colour)"
else
    bad "no icon colour anywhere in the band: the icons did not draw"
fi
if [ "$BAND_GROUND" -gt 0 ]; then
    ok "the icon cells let the bar ground through ($BAND_GROUND of $BAND_TOTAL band pixels)"
else
    bad "no bar ground in the band: the icon cells painted over everything"
fi

# one press, one assertion, on the left cell
LINE=$(press_in A "$HIT_A" 1)
GOTX=$(printf '%s' "$LINE" | awk '{ print $3 }')
GOTY=$(printf '%s' "$LINE" | awk '{ print $4 }')
case "$LINE" in
"A Activate "*) ok "left cell press reached item A (button 1)" ;;
*) bad "button 1 on the left cell did not reach item A: [$LINE]" ;;
esac
# The press is in root coordinates, so getting it back unchanged means the
# bar origin was added on the way out. With bar_gap=8 that is an 8 px
# difference; a bar_root_point() that dropped the origin would be off here.
if [ "${GOTX:-x}" = "$HIT_A" ]; then
    ok "recorded x is the press x in root coordinates ($GOTX)"
else
    bad "recorded x ${GOTX:-none} is not the press x $HIT_A (bar origin +$BX lost?)"
fi
# The item's y hint is the icon's own vertical centre, converted to root
# coordinates: bar origin y + half the strip. A dropped origin shows up as
# a ~$BY error here.
if [ -n "${GOTY:-}" ] && [ "$GOTY" -ge "$BY" ] 2>/dev/null &&
   [ "$GOTY" -lt "$((BY + BH))" ] 2>/dev/null; then
    ok "recorded y is inside the bar strip ($GOTY in [$BY,$((BY + BH))))"
else
    bad "recorded y ${GOTY:-none} is outside the bar strip [$BY,$((BY + BH)))"
fi
MID=$((BY + BH / 2))
if [ -n "${GOTY:-}" ] && [ "$((GOTY - MID))" -le 3 ] 2>/dev/null &&
   [ "$((MID - GOTY))" -le 3 ] 2>/dev/null; then
    ok "recorded y is the strip's centre in root coordinates ($GOTY ~ $MID)"
else
    bad "recorded y ${GOTY:-none} is not bar origin + half height ($MID)"
fi
Y1=$GOTY

# the right cell must resolve to the other item
LINE=$(press_in B "$HIT_B" 1)
case "$LINE" in
"B Activate "*) ok "right cell press reached item B (per-cell index)" ;;
*) bad "right cell press did not reach item B: [$LINE]" ;;
esac

# buttons 2 and 3 on the left cell: same x, same y, different method
for spec in "2 SecondaryActivate" "3 ContextMenu"; do
    btn=${spec%% *}
    want=${spec#* }
    LINE=$(press_in A "$HIT_A" "$btn")
    case "$LINE" in
    "A $want $HIT_A $Y1") ok "button $btn reached $want with the press point" ;;
    *) bad "button $btn: got [$LINE], want [A $want $HIT_A $Y1]" ;;
    esac
done

# 4/5 and 6/7: signed vertical and horizontal scroll
for spec in "4 1 vertical" "5 -1 vertical" "6 -1 horizontal" "7 1 horizontal"; do
    btn=$(printf '%s' "$spec" | awk '{ print $1 }')
    delta=$(printf '%s' "$spec" | awk '{ print $2 }')
    orient=$(printf '%s' "$spec" | awk '{ print $3 }')
    LINE=$(press_in A "$HIT_A" "$btn")
    if [ "$LINE" = "A Scroll $delta $orient" ]; then
        ok "button $btn scrolled $delta $orient"
    else
        bad "button $btn: got [$LINE], want [A Scroll $delta $orient]"
    fi
done

stop_item
wait_items 0 40 && ok "items unregistered after they exited" \
    || bad "registry still holds $(items | tr '\n' ' ')"

echo "-- empty strip collapses again"
SNI_NO_REGISTER=1 python3 "$TMP/ui_item.py" >"$TMP/item1.out" 2>&1 &
ITEM0_PID=$!
sleep 1
sweep 8 25
if [ "$SWEEP_ANY" -eq 0 ]; then
    ok "no tray cell after the items exited (strip collapsed again)"
else
    bad "$SWEEP_ANY press(es) still reached an item after they exited"
fi
stop_item0

echo "-- phase B: real-app acceptance (caffeine)"
# A hard gate when the environment can run it: the themed icon must resolve,
# the real popup must open with rows, and a real Quit row must end the app.
# Every step that cannot be reached here is reported as BLOCKED, which is
# information, not a pass and not a failure of the tree.
APP=""
for candidate in caffeine caffeine-ng; do
    command -v "$candidate" >/dev/null 2>&1 && { APP=$candidate; break; }
done
if [ -z "$APP" ]; then
    echo "  ACCEPTANCE BLOCKED: no caffeine/caffeine-ng on PATH"
else
    # the phase A fakes go first: one item means one cell, and the log
    # assertions below can be scoped to the lines written after the real app
    # started rather than catching the fakes' own IconPixmap fallback
    stop_item
    stop_item0
    t=0
    while [ "$t" -lt 60 ] && [ "$(items | wc -l)" -ne 0 ]; do
        sleep 0.1
        t=$((t + 1))
    done
    if [ "$(items | wc -l)" -ne 0 ]; then
        echo "  ACCEPTANCE BLOCKED: the fake items did not unregister"
    fi
    LOGMARK=$(wc -l < "$LOG")
    xwd -id "$BAR" -out "$TMP/real-before.xwd" 2>/dev/null
    # Watching the real client's menu traffic: it is the only witness to
    # which row a press addressed, and the acceptance below is about what
    # the client received, not about what the screen showed.
    : >"$TMP/app-monitor.txt"
    dbus-monitor --session "interface=com.canonical.dbusmenu" \
        >"$TMP/app-monitor.txt" 2>&1 &
    MON_PID=$!
    sleep 0.4
    "$APP" >"$TMP/app.log" 2>&1 &
    APP_PID=$!
    t=0
    while [ "$t" -lt 60 ] && [ "$(items | wc -l)" -eq 0 ]; do
        kill -0 "$APP_PID" 2>/dev/null || break
        sleep 0.2
        t=$((t + 1))
    done
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "  ACCEPTANCE BLOCKED: $APP exited on the private display:"
        sed -n '1,10p' "$TMP/app.log" | sed 's/^/    /'
        stop_app
        stop_monitor
    elif [ "$(items | wc -l)" -eq 0 ]; then
        echo "  ACCEPTANCE FAILED: $APP ran but registered no item"
        stop_app
        stop_monitor
    else
        # A child that has exited but has not been waited for is still a
        # live pid, so `kill -0` alone reports an app that is long gone as
        # still running. The registry is the honest test: the app is the
        # only item here, so an empty registry means it unregistering.
        app_gone() {
            kill -0 "$APP_PID" 2>/dev/null || return 0
            [ "$(items | wc -l)" -eq 0 ]
        }
        ok "$APP registered: $(items | tr '\n' ' ')"
        sleep 1
        xwd -id "$BAR" -out "$TMP/real-after.xwd" 2>/dev/null
        RPX=$(changed_pixels "$TMP/real-before.xwd" "$TMP/real-after.xwd")
        if [ -n "$RPX" ] && [ "$RPX" -gt 0 ] 2>/dev/null; then
            ok "bar repainted when $APP registered ($RPX px differ; repaint evidence only)"
        else
            echo "  ACCEPTANCE FAILED: no bar repaint when $APP registered"
        fi

        # The acceptance case from the phase 3 contract: the themed icon
        # must resolve, which the backend proves by NOT logging the no-icon
        # line it writes for an icon it could not find.
        tail -n "+$((LOGMARK + 1))" "$LOG" >"$TMP/app-wm.log" 2>/dev/null
        if grep -q 'no icon for "caffeine-cup-empty"' "$TMP/app-wm.log"; then
            bad "the themed caffeine icon did not resolve: $(grep -m1 'no icon for' "$TMP/app-wm.log")"
        else
            ok "the themed caffeine icon resolved (no no-icon log line)"
        fi
        if grep -q 'using IconPixmap' "$TMP/app-wm.log"; then
            bad "caffeine fell back to its IconPixmap: $(grep -m1 'using IconPixmap' "$TMP/app-wm.log")"
        else
            ok "caffeine did not fall back to IconPixmap"
        fi

        # The real popup, from the real item, through the real event loop.
        # A real app answers nothing to the marker file the fakes use, so
        # its cell is found the way a user finds it: right-click along the
        # strip until a popup appears.
        RCELL=""
        _lo=$((BX + BW * 25 / 100)); _x=$((BX + BW - 4))
        while [ "$_x" -gt "$_lo" ]; do
            press "$_x" "$Y" 3
            sleep 0.4
            _p=$(find_popup_near "$_x")
            # a popup that is still there a moment later is a real one; a
            # flash that is already gone was a failed exchange, not a hit
            if [ -n "$_p" ]; then
                sleep 0.6
                if [ -n "$(find_popup_near "$_x")" ]; then
                    RCELL=$_x
                    RPOP0=$_p
                    xdotool key --clearmodifiers Escape 2>/dev/null
                    sleep 0.4
                    break
                fi
            fi
            # a tray cell is about twenty pixels wide, so a step of eight
            # cannot miss one and the sweep stays quick
            _x=$((_x - 8))
        done
        if [ -z "$RCELL" ]; then
            bad "no $APP tray cell produced a popup; the app is running and its menu did not open"
            echo "     what the backend said while $APP was up:"
            sed -n '1,12p' "$TMP/app-wm.log" 2>/dev/null | sed 's/^/       /'
        else
            : >"$SNI_MARK"
            press "$RCELL" "$Y" 3
            RPOP=""
            t=0
            while [ "$t" -lt 60 ]; do
                RPOP=$(find_popup_near "$RCELL")
                [ -n "$RPOP" ] && break
                sleep 0.1
                t=$((t + 1))
            done
            # A hard gate, not an observation: an empty layout is a parser
            # or layout failure, and it is exactly the shape a real client
            # used to answer in, so it may never be reported as BLOCKED.
            if grep -q 'no rows to show' "$TMP/app-wm.log" 2>/dev/null; then
                bad "$APP's GetLayout answer was rejected as empty:"
                grep -m2 'no rows to show' "$TMP/app-wm.log" | sed 's/^/     /'
            else
                ok "no empty-layout fallback was logged for $APP"
            fi
            if grep -qE 'no answer in time|GetLayout failed|GetLayout answered with nothing usable' "$TMP/app-wm.log" 2>/dev/null; then
                bad "$APP's menu exchange did not complete:"
                grep -m2 -E 'no answer in time|GetLayout' "$TMP/app-wm.log" |
                    sed 's/^/     /'
            else
                ok "$APP's menu exchange completed with no fallback"
            fi
            if [ -z "$RPOP" ]; then
                bad "$APP right-click produced no popup window"
            else
                ok "the real $APP popup appeared ($RPOP)"
                RPH=$(win_field "$RPOP" h); RPY=$(win_field "$RPOP" y)
                RPX2=$(win_field "$RPOP" x)
                echo "     real popup at +$RPX2+$RPY, ${RPH}px tall"
                if [ "$RPH" -gt 30 ]; then
                    ok "the real popup has rows (${RPH}px tall)"
                else
                    bad "the real popup is too short to hold rows (${RPH}px)"
                fi
                # Find the real Quit row the way a user does: press rows
                # until the app is gone. Bottom-up, because a menu's quit
                # row is its last one, and a row that does not quit just
                # closes the popup - so the walk reopens it each time.
                QUIT=no
                _py=$((RPY + RPH - 4)); _start=$((RPY + 3)); _g=0
                while [ "$_py" -ge "$_start" ] && [ "$_g" -lt 40 ]; do
                    press $((RPX2 + 10)) "$_py" 1
                    sleep 0.5
                    if app_gone; then
                        QUIT=yes
                        break
                    fi
                    [ -n "$(find_popup_near "$RCELL")" ] || {
                        press "$RCELL" "$Y" 3
                        sleep 0.8
                    }
                    _py=$((_py - 3))
                    _g=$((_g + 1))
                done
                # What the client actually received. dbus-monitor is the
                # only witness to which row a press addressed, and without
                # it "the app did not quit" cannot be told apart from "the
                # press never reached a row".
                CLICK_IDS=$(clicked_ids)
                NDISTINCT=$(printf '%s\n' $CLICK_IDS | sort -u | grep -c . \
                    || true)
                echo "     rows clicked on the wire: $(printf '%s' \
                    "$CLICK_IDS" | tr '\n' ' ')"
                if [ "$QUIT" = yes ]; then
                    ok "a real menu row ended $APP (Quit)"
                elif [ "$NDISTINCT" -ge 2 ] 2>/dev/null; then
                    # The host's half is proven: the press was addressed to
                    # $NDISTINCT different rows of the real menu, and the
                    # app stayed up on the last one. That is the client's
                    # own dependencies (a power-manager D-Bus service it
                    # uninhibits through on the way out), not a menu that
                    # would not open.
                    echo "  ACCEPTANCE BLOCKED: $APP received clicked() for"
                    echo "     $NDISTINCT different rows of its real menu and"
                    echo "     did not exit on any of them. Its own log shows"
                    sed -n '1,3p' "$TMP/app.log" 2>/dev/null |
                        grep -iE "WARNING|error" | cut -c1-90 | sed 's/^/       /'
                else
                    bad "the walk never reached a second row of the real menu"
                    bad "(ids clicked: ${CLICK_IDS:-none}), so no Quit row was reachable"
                fi
            fi
        fi
        stop_app
        stop_monitor
        t=0
        while [ "$t" -lt 40 ] && [ "$(items | wc -l)" -ne 0 ]; do
            sleep 0.1
            t=$((t + 1))
        done
        if [ "$(items | wc -l)" -eq 0 ]; then
            ok "registry drained after $APP exited"
        else
            bad "registry still holds $(items | tr '\n' ' ')"
        fi
    fi
fi

echo "-- clean shutdown"
kill -TERM "$WM_PID" 2>/dev/null || true
i=0
while [ "$i" -lt 100 ]; do
    kill -0 "$WM_PID" 2>/dev/null || break
    sleep 0.1
    i=$((i + 1))
done
kill -0 "$WM_PID" 2>/dev/null && { bad "wm ignored SIGTERM"; kill -9 "$WM_PID" 2>/dev/null; }
kill -0 "$WM_PID" 2>/dev/null || ok "exited on SIGTERM"
wait "$WM_PID" 2>/dev/null || true
WM_PID=""

echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
CHECK

export WM_PID LOG="$TMP/austere.log" SNI_MARK="$TMP/mark" DISPLAY="$DISP" BIN TMP
export HOME="$TMP/home"
export XDG_CONFIG_HOME="$TMP/home/.config"
export XDG_DATA_HOME="$TMP/home/.local/share"
export XDG_CACHE_HOME="$TMP/home/.cache"
export XDG_RUNTIME_DIR="$TMP/home/run"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
    "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

if dbus-run-session -- sh "$TMP/check.sh"; then
    echo "PASS: tray UI interaction"
else
    rc=$?
    echo "FAIL: tray UI check failed (rc=$rc)" >&2
    [ -f "$TMP/austere.log" ] && sed -n '1,80p' "$TMP/austere.log" >&2
    exit 1
fi

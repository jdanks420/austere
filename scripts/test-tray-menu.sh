#!/bin/sh
# Phase 3 tray menu acceptance: the real popup, driven through the real
# event loop, against fake StatusNotifierItem + com.canonical.dbusmenu
# peers on a private display and a private session bus.
#
# Same harness shape as scripts/test-tray-ui.sh: throwaway HOME and XDG
# dirs, a private Xvfb, a private dbus-run-session, one process per fake
# item so an item can be killed, and every child bounded and reaped on all
# exit paths. Nothing here touches the real display, the real session bus
# or the real config.
#
# How the checks avoid guessing:
#   - a tray cell is found by pointer sweep, not by re-deriving the bar's
#     layout maths;
#   - the popup window is whichever window appears when a menu opens;
#   - a row is found by probing downwards and watching which id the peer
#     records, so hit-testing is observed, not derived from the designer's
#     row height;
#   - the row icon is proven by a pixel difference between two popups that
#     are identical apart from that one icon. Pixel assertions are always
#     scoped to the popup window, never to the bar.
#
# Usage: scripts/test-tray-menu.sh [path-to-austere]
#
# Needs Xvfb, dbus-run-session, dbus-send, xdotool, xwininfo, xwd, convert,
# awk and python3 with GObject introspection. Any of those missing is an
# explicit SKIP (exit 77), so `make test` never fails on a UI-only
# dependency.

set -eu

BIN=${1:-${AUSTERE_BIN:-./austere}}
case "$BIN" in
    /*) ;;
    *)  BIN="$PWD/$BIN" ;;
esac

missing=
for tool in Xvfb dbus-run-session dbus-send xdotool xwininfo xwd convert awk; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
python3 -c "import gi; gi.require_version('Gio', '2.0')" >/dev/null 2>&1 ||
    missing="$missing python3-gi"
if [ -n "$missing" ]; then
    echo "SKIP: missing test dependencies:$missing (tray menu check not run)" >&2
    exit 77
fi
[ -x "$BIN" ] || { echo "FAIL: no executable at $BIN" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/austere-traymenu.XXXXXX")
XVFB_PID=""

cleanup() {
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# ---- private X display ------------------------------------------------
# A third disjoint range, so `make -j test` cannot have the three
# harnesses race for the same free socket: 90-109 watcher, 110-129 bar UI,
# 130-149 this one. Sockets are still scanned for a free one first.
DISP=""
n=130
while [ "$n" -lt 150 ]; do
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

# ---- one fake peer ----------------------------------------------------
# ITEM_TAG selects which item this process exports, so each item is its own
# pid and an item can be killed to prove the popup goes with it.
cat >"$TMP/menu_item.py" <<'ITEM'
"""One fake StatusNotifierItem, with a com.canonical.dbusmenu menu, on one
private bus. Every call is logged with the object path it arrived on, so
the checks can assert which object the WM addressed.

  ay    Ayatana style: the SNI and its menu are one object, the item
        carries an IconThemePath, and row 0 asks for an icon that only
        exists inside that theme
  kde   a standard KDE item: SNI at /StatusNotifierItem, menu at
        /MenuBar, the same row icon, but no IconThemePath
  noic  the same menu with row 0's icon-name left out
  bad   its menu errors, so the item's own ContextMenu is the fallback
  none  no Menu property at all, so the fallback is immediate
  sub   its menu is a sub-path of its own object, the third shape there is
  leg   its menu hands the children over in a "children" property instead
        of in the layout's third field, the one non-canonical shape the
        backend still has to understand

Test scaffolding."""

import os
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

TAG = os.environ["ITEM_TAG"]
LOG = os.environ["SNI_MARK"]
THEME = os.environ.get("PROBE_THEME", "")

SNI = "org.kde.StatusNotifierItem"
MENU = "com.canonical.dbusmenu"
PROPS = "org.freedesktop.DBus.Properties"

# tag: (service, sni path, menu path, IconThemePath, mode, row0 icon name)
ITEMS = {
    "ay":   ("org.austere.TrayAy", "/TrayAy", "/TrayAy", THEME, "good",
             "appointment-missed"),
    "kde":  ("org.austere.TrayKde", "/StatusNotifierItem", "/MenuBar", "",
             "good", "appointment-missed"),
    "noic": ("org.austere.TrayNoIc", "/TrayNoIc", "/TrayNoIc", "", "good",
             ""),
    "bad":  ("org.austere.TrayBad", "/TrayBad", "/TrayBad", "", "error",
             "appointment-missed"),
    "none": ("org.austere.TrayNone", "/TrayNone", "", "", "good",
             "appointment-missed"),
    # a sub-path of the item's own object, which is what a real Ayatana
    # indicator advertises
    "sub":  ("org.austere.TraySub", "/TraySub", "/TraySub/Menu", "", "good",
             "appointment-missed"),
    "leg":  ("org.austere.TrayLeg", "/TrayLeg", "/TrayLeg", "", "legacy",
             "appointment-missed"),
}
SERVICE, SNI_PATH, MENU_PATH, ITEM_THEME, MODE, ROW_ICON = ITEMS[TAG]

# A themed name for the item's own icon. caffeine-cup-empty lives in
# hicolor too, so it proves resolution rather than the theme path; the
# absence of a no-icon log line is what the check relies on.
ITEM_ICON = "caffeine-cup-empty"

SNI_PROPS = ["Category", "Id", "Title", "Status", "IconName",
             "AttentionIconName", "ItemIsMenu", "Menu", "IconThemePath",
             "IconPixmap"]
PROPS_XML = "".join(
    '  <property name="%s" type="%s" access="read"/>\n'
    % (k, {"IconPixmap": "a(iiay)"}.get(k, "s")) for k in sorted(SNI_PROPS))

SNI_IFACES = """<interface name="org.freedesktop.DBus.Properties">
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
%s <method name="Activate">
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
</interface>""" % PROPS_XML

MENU_IFACES = """<interface name="com.canonical.dbusmenu">
 <method name="Event">
  <arg name="id" type="i" direction="in"/>
  <arg name="eventId" type="s" direction="in"/>
  <arg name="data" type="v" direction="in"/>
  <arg name="timestamp" type="u" direction="in"/>
 </method>
 <method name="AboutToShow">
  <arg name="id" type="i" direction="in"/>
  <arg name="needUpdate" type="b" direction="out"/>
 </method>
 <method name="GetLayout">
  <arg name="parentId" type="i" direction="in"/>
  <arg name="recursionDepth" type="i" direction="in"/>
  <arg name="propertyNames" type="as" direction="in"/>
  <arg name="revision" type="u" direction="out"/>
  <arg name="layout" type="%s" direction="out"/>
 </method>
 <signal name="LayoutUpdated">
  <arg name="revision" type="u"/>
  <arg name="parent" type="i"/>
 </signal>
 <signal name="ItemsPropertiesUpdated">
  <arg name="updatedProps" type="a(ia{sv})"/>
  <arg name="removedProps" type="a(ias)"/>
 </signal>
</interface>""" % ("(ia{sv})" if MODE == "legacy" else "(ia{sv}av)")


def log(line):
    with open(LOG, "a") as handle:
        handle.write("%s\n" % line)


def sni_values():
    return {
        "Category": GLib.Variant("s", "ApplicationStatus"),
        "Id": GLib.Variant("s", TAG),
        "Title": GLib.Variant("s", "fake item " + TAG),
        "Status": GLib.Variant("s", "Active"),
        "IconName": GLib.Variant("s", ITEM_ICON),
        "AttentionIconName": GLib.Variant("s", ""),
        "ItemIsMenu": GLib.Variant("b", False),
        "Menu": GLib.Variant("o", MENU_PATH) if MENU_PATH
        else GLib.Variant("o", "/NO_DBUSMENU"),
        "IconThemePath": GLib.Variant("s", ITEM_THEME),
        "IconPixmap": GLib.Variant("a(iiay)", ()),
    }


def sni_call(conn, sender, path, iface, method, params, invocation):
    if method == "GetAll":
        invocation.return_value(GLib.Variant("(a{sv})", (sni_values(),)))
    elif method == "Get":
        wanted = params.unpack()[1]
        invocation.return_value(GLib.Variant(
            "(v)", (sni_values().get(wanted, GLib.Variant("s", "")),)))
    elif method in ("Activate", "SecondaryActivate", "ContextMenu"):
        log("SNI %s %s %s" % (SNI_PATH, method, params.print_(True)))
        invocation.return_value(None)
    elif method == "Scroll":
        log("SNI %s Scroll" % SNI_PATH)
        invocation.return_value(None)
    else:
        log("SNI %s UNKNOWN %s" % (SNI_PATH, method))
        invocation.return_dbus_error(
            "org.freedesktop.DBus.Error.UnknownMethod", method)


def sni_prop_get(conn, path, iface, name):
    return sni_values().get(name, GLib.Variant("s", ""))


def layout_reply():
    """The canonical com.canonical.dbusmenu GetLayout answer: a u revision
    and a (ia{sv}av) layout, so the rows are the struct's THIRD field and
    each of them is a variant wrapping one child (ia{sv}av)."""

    def row(**kw):
        return {k.replace("_", "-"): GLib.Variant(*v)
                for k, v in kw.items()}

    first = row(label=("s", "_Quit"), enabled=("b", True), visible=("b", True),
                type=("s", "standard"))
    if ROW_ICON:
        first["icon-name"] = GLib.Variant("s", ROW_ICON)
    rows = [
        first,
        row(label=("s", ""), type=("s", "separator")),
        row(label=("s", "_Keep __literal"), enabled=("b", True),
            toggle_type=("s", "checkmark"), toggle_state=("i", 0)),
        row(label=("s", "Mode B"), toggle_type=("s", "radio"),
            toggle_state=("i", 1)),
        row(label=("s", "Unavailable"), enabled=("b", False)),
        # the backend must skip both of these
        row(label=("s", "Hidden"), visible=("b", False)),
        # a submenu, carrying a real child: nothing below a submenu may
        # ever become a row, and no press may ever reach id 171
        row(label=("s", "More"), type=("s", "submenu")),
    ]
    kids = []
    for ident, props in zip((11, 12, 13, 14, 15, 16, 17), rows):
        below = [GLib.Variant("(ia{sv}av)", (171, row(label=("s", "SubRow")),
                                              []))] if ident == 17 else []
        kids.append(GLib.Variant("(ia{sv}av)", (ident, props, below)))
    root = {"label": GLib.Variant("s", "Fake Menu " + TAG)}
    return GLib.Variant("(u(ia{sv}av))", (7, (0, root, kids)))


def legacy_reply():
    """The non-canonical shape some clients ship: the children in a
    "children" property rather than in the layout's third field. The
    backend keeps reading it, but only as a fallback."""

    def row(**kw):
        return {k.replace("_", "-"): GLib.Variant(*v)
                for k, v in kw.items()}

    first = row(label=("s", "_Quit"), enabled=("b", True), visible=("b", True),
                type=("s", "standard"))
    if ROW_ICON:
        first["icon-name"] = GLib.Variant("s", ROW_ICON)
    rows = [
        first,
        row(label=("s", ""), type=("s", "separator")),
        row(label=("s", "Mode B"), toggle_type=("s", "radio"),
            toggle_state=("i", 1)),
        row(label=("s", "Unavailable"), enabled=("b", False)),
        row(label=("s", "Hidden"), visible=("b", False)),
    ]
    children = list(zip((11, 12, 13, 14, 15), rows))
    root = {"label": GLib.Variant("s", "Legacy Menu"),
            "children": GLib.Variant("a(ia{sv})", children)}
    return GLib.Variant("(u(ia{sv}))", (7, (0, root)))


def menu_call(conn, sender, path, iface, method, params, invocation):
    if MODE == "error":
        log("MENU %s ERROR %s" % (MENU_PATH, method))
        invocation.return_dbus_error(
            "com.canonical.DBusMenu.Error.NotAvailable", method)
        return
    if method == "AboutToShow":
        log("MENU %s AboutToShow" % MENU_PATH)
        invocation.return_value(GLib.Variant("(b)", (True,)))
        return
    if method == "GetLayout":
        log("MENU %s GetLayout" % MENU_PATH)
        invocation.return_value(legacy_reply() if MODE == "legacy"
                               else layout_reply())
        return
    if method == "Event":
        args = params.unpack()
        # printed whole, so the variant's contained type is visible
        log("MENU %s Event %s id=%d %s" % (MENU_PATH, args[1], args[0],
            params.print_(True)))
        invocation.return_value(None)
        return
    log("MENU %s UNKNOWN %s" % (MENU_PATH, method))
    invocation.return_dbus_error(
        "org.freedesktop.DBus.Error.UnknownMethod", method)


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(
    "<node>%s%s</node>" % (SNI_IFACES, MENU_IFACES))
conn.register_object(SNI_PATH, node.lookup_interface(PROPS), sni_call,
                     sni_prop_get, None)
conn.register_object(SNI_PATH, node.lookup_interface(SNI), sni_call,
                     sni_prop_get, None)
if MENU_PATH:
    conn.register_object(MENU_PATH, node.lookup_interface(MENU), menu_call,
                         None, None)
conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
               "org.freedesktop.DBus", "RequestName",
               GLib.Variant("(su)", (SERVICE, 4)),
               GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 5000, None)
conn.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
               "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
               GLib.Variant("(s)", (SERVICE + SNI_PATH,)), None,
               Gio.DBusCallFlags.NONE, 5000, None)
log("UP %s" % TAG)


if MENU_PATH and MODE in ("good", "legacy"):
    GLib.timeout_add(1200, lambda: (
        conn.emit_signal(None, MENU_PATH, MENU, "LayoutUpdated",
                         GLib.Variant("(ui)", (8, 0))), True)[1])

loop = GLib.MainLoop()
# A backstop only: the harness owns these processes and reaps them on every
# exit path. It has to be far longer than the whole check, because a peer
# that quit early would take its item with it and every later phase would
# be testing a dead registry rather than the code.
GLib.timeout_add(900000, loop.quit)
loop.run()
ITEM

# ---- the checks, inside a private session bus -------------------------
cat >"$TMP/check.sh" <<'CHECK'
set -u
PASS=0
FAIL=0
WM_PID=""
POPUP=""
CELL=""
CELL_TAG=""

ok()  { PASS=$((PASS + 1)); echo "  ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $*"; }

# SIGTERM, then SIGKILL, then reap: a bare wait would wedge the harness on
# a process that ignores the first signal.
stop_pid() {   # stop_pid <pid>
    [ -n "$1" ] || return 0
    kill "$1" 2>/dev/null || true
    _t=0
    while [ "$_t" -lt 30 ] && kill -0 "$1" 2>/dev/null; do
        sleep 0.1
        _t=$((_t + 1))
    done
    kill -9 "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
    return 0
}

stop_item() {  # stop_item <tag>
    eval "_p=\$PID_$1"
    stop_pid "$_p"
    eval "PID_$1="
}

cleanup_here() {

    for _t in ay kde noic bad none sub leg; do
        eval "_p=\$PID_$_t"
        stop_pid "$_p"
    done
    # The wm gets the same bounded stop as the fakes: a phase that fails an
    # assertion must not leave it holding the harness's X display, and a wm
    # that ignored SIGTERM would survive a plain kill too.
    [ -n "$WM_PID" ] && kill -TERM "$WM_PID" 2>/dev/null
    _t=0
    while [ "$_t" -lt 30 ] && [ -n "$WM_PID" ] &&
          kill -0 "$WM_PID" 2>/dev/null; do
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

wait_items() {   # wait_items <count> <deciseconds>
    _want=$1; _t=0
    while [ "$_t" -lt "$2" ]; do
        [ "$(items | wc -l)" -eq "$_want" ] && return 0
        sleep 0.1
        _t=$((_t + 1))
    done
    return 1
}

find_bar() {
    xwininfo -root -tree 2>/dev/null | awk '/^[[:space:]]+0x/ {
        if (match($0, /[0-9]+x[0-9]+\+[0-9-]+\+[0-9-]+/)) {
            g = substr($0, RSTART, RLENGTH)
            split(g, d, /[x+]/)
            if (d[1] >= 900 && d[2] <= 48) { print $1; exit }
        }
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

mapped_windows() {
    xwininfo -root -tree 2>/dev/null | awk '/^[[:space:]]+0x/ { print $1 }'
}

# The popup is the one viewable root child that is popup-sized and sits
# near the cell that was pressed. Identifying it by geometry rather than by
# "a window that appeared" matters because X reuses window ids: a popup
# destroyed by Escape and reopened a moment later can come back with the
# same id, which a before/after diff would miss.
find_popup() {   # find_popup <cell-x>
    xwininfo -root -tree 2>/dev/null | awk -v cx="$1" '
        /^[[:space:]]+0x/ {
            if (!match($0, /[0-9]+x[0-9]+\+[0-9-]+\+[0-9-]+/)) next
            g = substr($0, RSTART, RLENGTH)
            split(g, d, /[x+]/)
            w = d[1] + 0; h = d[2] + 0; x = d[3] + 0
            # the bar is 1024 wide, so a width bound rules it out
            if (w < 20 || w > 600) next
            if (h < 20) next
            if (x < cx - 40 || x > cx + 600) next
            print $1
            exit
        }'
}

popup_mapped() { find_popup "$1" | grep -qx "$POPUP"; }


press() { xdotool mousemove "$1" "$2" click "$3" 2>/dev/null; }

# grep -c prints 0 and exits 1 when nothing matched, so a trailing
# `|| echo 0` would append a second 0 and every comparison below would
# fail on a non-numeric value.
count() { grep -c "$1" "$2" 2>/dev/null || true; }

changed_pixels() {
    convert "$1" "$2" -compose difference -composite -colorspace Gray \
        -threshold 1% -format "%[fx:mean*w*h]" info: 2>/dev/null
}

popup_gone() { [ -n "$POPUP" ] && ! popup_mapped "$CELL"; }

# The sweep names its variables after the tag in upper case, so map rather
# than eval: a tag is lower case and a variable name never is.
cell_of() {     # cell_of <tag>
    case "$1" in
    ay)   echo "${CELL_AY:-}" ;;
    kde)  echo "${CELL_KDE:-}" ;;
    noic) echo "${CELL_NOIC:-}" ;;
    bad)  echo "${CELL_BAD:-}" ;;
    none) echo "${CELL_NONE:-}" ;;
    sub)  echo "${CELL_SUB:-}" ;;
    leg)  echo "${CELL_LEG:-}" ;;
    *)    echo "" ;;
    esac
}

# What the item writes in its own mark when a press reaches it, so a cell
# can be recognised by the item answering rather than by a coordinate.
item_prefix() {   # item_prefix <tag>
    case "$1" in
    ay)   echo "SNI /TrayAy Activate" ;;
    kde)  echo "SNI /StatusNotifierItem Activate" ;;
    noic) echo "SNI /TrayNoIc Activate" ;;
    bad)  echo "SNI /TrayBad Activate" ;;
    none) echo "SNI /TrayNone Activate" ;;
    sub)  echo "SNI /TraySub Activate" ;;
    leg)  echo "SNI /TrayLeg Activate" ;;
    *)    echo "" ;;
    esac
}

# The strip's cells move while the run goes on: a cpu or ram percentage
# gaining a digit shifts every cell to its left, so a recorded x goes
# stale and a later press reaches the wrong item or nothing at all. This
# finds the wanted item's cell again near where it last was - the way a
# user looks before clicking - and walks a couple of pixels inwards, so
# what it returns is an interior point rather than a boundary one.
cell_now() {   # cell_now <tag> -> interior x, or nothing
    _want=$(item_prefix "$1")
    [ -n "$_want" ] || return 1
    _from=$(cell_of "$1")
    [ -n "$_from" ] || return 1
    _lo=$((BX + BW * 20 / 100))
    for _d in 0 2 4 6 8 10 12 14 16 -2 -4 -6 -8 -10 -12 -14 -16; do
        _try=$((_from + _d))
        [ "$_try" -le "$_lo" ] && continue
        : >"$SNI_MARK"
        press "$_try" "$Y" 1
        sleep 0.12
        case "$(head -1 "$SNI_MARK" 2>/dev/null)" in
        "$_want"*)
            _x=$_try
            _k=0
            while [ "$_k" -lt 6 ]; do
                : >"$SNI_MARK"
                press $((_x - 2)) "$Y" 1
                sleep 0.12
                case "$(head -1 "$SNI_MARK" 2>/dev/null)" in
                "$_want"*) _x=$((_x - 2)) ;;
                *) break ;;
                esac
                _k=$((_k + 1))
            done
            echo "$_x"
            return 0
            ;;
        esac
    done
    return 1
}

# What appears anywhere in an item's own mark - its own object path - so a
# press can be confirmed to have reached the item it was aimed at, whichever
# of its methods answered.
item_token() {   # item_token <tag>
    case "$1" in
    ay)   echo "TrayAy" ;;
    # a standard KDE item's menu is on another object, so either its own
    # path or its menu path counts as proof the press reached it
    kde)  echo "StatusNotifierItem\\|MenuBar" ;;
    noic) echo "TrayNoIc" ;;
    bad)  echo "TrayBad" ;;
    none) echo "TrayNone" ;;
    sub)  echo "TraySub" ;;
    leg)  echo "TrayLeg" ;;
    *)    echo "" ;;
    esac
}

# Find the cell, press, and confirm the item answered. The strip can move
# between finding a cell and pressing it - a cpu or ram percentage gaining a
# digit shifts every cell to its left - so a press that reached some other
# item is not evidence about this one. The press is retried against a
# freshly found cell until the wanted item is the one that answers, which is
# also what a person does: they look, then they click, then they look again.
press_item() {   # press_item <tag> <button> [hint-x] -> the x pressed, or nothing
    _tag=$1; _btn=$2; _hint=${3:-}; _tok=$(item_token "$_tag"); _a=0
    [ -n "$_tok" ] || return 1
    while [ "$_a" -lt 4 ]; do
        if [ -n "$_hint" ]; then
            _x=$_hint
        else
            _x=$(cell_now "$_tag") || _x=""
        fi
        if [ -n "$_x" ]; then
            : >"$SNI_MARK"
            press "$_x" "$Y" "$_btn"
            sleep 0.3
            if grep -q "$_tok" "$SNI_MARK" 2>/dev/null; then
                echo "$_x"
                return 0
            fi
        fi
        # The cell it missed was stale, so every later attempt re-finds it.
        _a=$((_a + 1))
        _hint=""
    done
    return 1
}

# Open the menu at a cell and remember which window it is. The cell is
# found again first: a phase that ran a minute ago may remember an x the
# strip has since moved.
open_menu() {   # open_menu <tag> <label>
    _x=$(press_item "$1" 3) || _x=""
    if [ -z "$_x" ]; then
        bad "$2: no press on the $1 item's cell reached that item"
        return 1
    fi
    CELL="$_x"
    CELL_TAG="$1"
    _t=0
    while [ "$_t" -lt 50 ]; do
        POPUP=$(find_popup "$_x")
        [ -n "$POPUP" ] && break
        sleep 0.1
        _t=$((_t + 1))
    done
    if [ -n "$POPUP" ]; then
        ok "$2: the popup window appeared ($POPUP)"
    else
        bad "$2: no popup window appeared"
        echo "     root children now: $(xwininfo -root -tree 2>/dev/null |
            grep -E '^[[:space:]]+0x' | sed 's/^[[:space:]]*//' | cut -c1-60 |
            tr '\n' '|')"
        echo "     searching near cell x=$_x; the bar is $BAR"
        echo "     what the item saw: $(tr '\n' '|' <"$SNI_MARK" 2>/dev/null |
            cut -c1-200)"
        echo "     what the wm said: $(grep -i tray "$LOG" 2>/dev/null |
            tail -2 | tr '\n' '|' | cut -c1-200)"
        POPUP=""
        return 1
    fi
    sleep 0.5
    return 0
}

# Reopen after a press that closed the menu, waiting for the window to be
# back before the next probe: without the wait the probe would be measuring
# a screen with no popup on it. The cell is re-pressed through press_item,
# so a strip that moved while the menu was open cannot silently leave the
# next probe measuring nothing.
reopen_popup() {
    _x=$(press_item "${CELL_TAG:-}" 3 "$CELL") || _x=""
    if [ -z "$_x" ]; then
        return 1
    fi
    CELL="$_x"
    _t=0
    while [ "$_t" -lt 50 ]; do
        _p=$(find_popup "$CELL")
        if [ -n "$_p" ]; then
            # a reopened popup can come back with a different id, and the
            # liveness check compares against the one we remember
            POPUP=$_p
            return 0
        fi
        sleep 0.1
        _t=$((_t + 1))
    done
    return 1
}

# Leave no popup behind, so one phase cannot consume the next phase's
# presses. Escape first, then an outside press if that was not enough.
close_popup() {
    [ -n "$POPUP" ] || return 0
    popup_mapped "$CELL" || return 0
    xdotool key --clearmodifiers Escape 2>/dev/null
    sleep 0.4
    if popup_mapped "$CELL"; then
        press 512 400 1
        sleep 0.4
    fi
    return 0
}

echo "-- phase A: the fake items register"
CONF_DIR="$HOME/.config/austere"
mkdir -p "$CONF_DIR"
cat >"$CONF_DIR/austere.conf" <<'CONF'
[bar]
position = "top"
bar_bg = "#1a1a1a"
bar_fg = "#cccccc"
bar_gap = 0
modules_left = ["workspaces", "layout"]
modules_center = ["title"]
modules_right = ["tray", "cpu", "ram", "clock"]
CONF
: >"$SNI_MARK"
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
    grep -q 'boolean true' && ok "the watcher name is owned" \
    || { bad "no watcher name"; sed -n '1,40p' "$LOG"; exit 1; }

for t in ay kde noic bad none sub leg; do
    ITEM_TAG=$t python3 "$TMP/menu_item.py" >>"$TMP/fake-$t.log" 2>&1 &
    eval "PID_$t=\$!"
done
if wait_items 7 150; then ok "all seven fake items registered"; else
    bad "items never reached 7: $(items | tr '\n' ' ')"; exit 1
fi
sleep 1.2

# The item icon resolved rather than falling back: the backend logs both
# failure shapes, and their absence is the evidence.
if grep -q 'no icon for' "$LOG"; then
    bad "an item icon did not resolve: $(grep -m1 'no icon for' "$LOG")"
else
    ok "every item icon resolved (no no-icon log line)"
fi
if grep -q 'using IconPixmap' "$LOG"; then
    bad "an item fell back to IconPixmap: $(grep -m1 'using IconPixmap' "$LOG")"
else
    ok "no item fell back to IconPixmap"
fi

BAR=$(find_bar)
[ -n "$BAR" ] && ok "located the bar window $BAR" || { bad "no bar window"; exit 1; }
BX=$(win_field "$BAR" x); BY=$(win_field "$BAR" y)
BW=$(win_field "$BAR" w); BH=$(win_field "$BAR" h)
Y=$((BY + BH / 2))
echo "     bar at +$BX+$BY, ${BW}x${BH}; pressing at y=$Y"

echo "-- phase B: find every tray cell by pointer sweep"
CELL_AY=""; CELL_KDE=""; CELL_NOIC=""; CELL_BAD=""; CELL_NONE=""
CELL_SUB=""; CELL_LEG=""
_lo=$((BX + BW * 20 / 100)); _x=$((BX + BW - 4))
while [ "$_x" -gt "$_lo" ]; do
    : >"$SNI_MARK"
    press "$_x" "$Y" 1
    sleep 0.12
    case "$(head -1 "$SNI_MARK" 2>/dev/null)" in
    "SNI /TrayAy Activate"*)            [ -n "$CELL_AY" ]   || CELL_AY=$_x ;;
    "SNI /StatusNotifierItem Activate"*) [ -n "$CELL_KDE" ]  || CELL_KDE=$_x ;;
    "SNI /TrayNoIc Activate"*)          [ -n "$CELL_NOIC" ] || CELL_NOIC=$_x ;;
    "SNI /TrayBad Activate"*)           [ -n "$CELL_BAD" ]  || CELL_BAD=$_x ;;
    "SNI /TrayNone Activate"*)          [ -n "$CELL_NONE" ] || CELL_NONE=$_x ;;
    "SNI /TraySub Activate"*)           [ -n "$CELL_SUB" ]  || CELL_SUB=$_x ;;
    "SNI /TrayLeg Activate"*)           [ -n "$CELL_LEG" ]  || CELL_LEG=$_x ;;
    esac
    _x=$((_x - 4))
done
for t in ay kde noic bad none sub leg; do
    c=$(cell_of "$t")
    [ -n "$c" ] && ok "found the $t cell at x=$c" || bad "no cell for $t"
done
[ -n "$CELL_AY" ] || { bad "cannot continue without a cell"; exit 1; }

# The sweep records a cell's rightmost sampled pixel, which is one step from
# the boundary with its neighbour - and the strip's cells move whenever a
# numeric module beside them changes width, a CPU percentage gaining a digit
# shifts every cell to its left. A boundary pixel is then answered by the
# other item, or by nothing at all, so every cell is walked inwards first:
# the point kept is the deepest one that still answers for the same item.
refine_cell() {   # refine_cell <tag> <from-x> -> interior x
    _t=$1; _x=$2; _keep=$2; _d=0
    while [ "$_d" -lt 12 ]; do
        : >"$SNI_MARK"
        press $((_x - 2)) "$Y" 1
        sleep 0.12
        _line=$(head -1 "$SNI_MARK" 2>/dev/null)
        # A press that reached nothing ends the walk: an empty line is not
        # the same item, and treating it as one would walk the cell out of
        # existence and record a point that answers for no item at all.
        case "$_t:$_line" in
        ay:"SNI /TrayAy Activate"*|kde:"SNI /StatusNotifierItem Activate"*|\
        noic:"SNI /TrayNoIc Activate"*|bad:"SNI /TrayBad Activate"*|\
        none:"SNI /TrayNone Activate"*|sub:"SNI /TraySub Activate"*|\
        leg:"SNI /TrayLeg Activate"*) ;;
        *) break ;;
        esac
        _keep=$((_x - 2))
        _x=$((_x - 2))
        _d=$((_d + 1))
    done
    echo "$_keep"
}
for t in ay kde noic bad none sub leg; do
    c=$(cell_of "$t")
    [ -n "$c" ] || continue
    r=$(refine_cell "$t" "$c")
    case "$t" in
    ay)   CELL_AY=$r ;;
    kde)  CELL_KDE=$r ;;
    noic) CELL_NOIC=$r ;;
    bad)  CELL_BAD=$r ;;
    none) CELL_NONE=$r ;;
    sub)  CELL_SUB=$r ;;
    leg)  CELL_LEG=$r ;;
    esac
done
echo "     cells walked inwards: ay=$CELL_AY kde=$CELL_KDE noic=$CELL_NOIC"
echo "                       bad=$CELL_BAD none=$CELL_NONE sub=$CELL_SUB leg=$CELL_LEG"

echo "-- phase C: a right-click opens the real popup and hit-tests"
: >"$SNI_MARK"
if open_menu ay "the Ayatana item"; then
    PX=$(win_field "$POPUP" x); PY=$(win_field "$POPUP" y)
    PW=$(win_field "$POPUP" w); PH=$(win_field "$POPUP" h)
    echo "     popup at +$PX+$PY, ${PW}x${PH}"
    [ "$PW" -gt 20 ] && [ "$PH" -gt 20 ] \
        && ok "the popup has a real size (${PW}x${PH})" \
        || bad "the popup has no usable size"
    xwd -id "$POPUP" -out "$TMP/ay-popup.xwd" 2>/dev/null

    # Walk down the popup until a press activates a row. The first band
    # that does is the first row, so the id proves which row was hit
    # without assuming anything about the row height.
    HIT_Y=""; HIT_ID=""
    _py=$((PY + 2)); _end=$((PY + PH - 2))
    while [ "$_py" -le "$_end" ]; do
        : >"$SNI_MARK"
        press $((PX + 6)) "$_py" 1
        sleep 0.16
        _line=$(grep -m1 "Event clicked" "$SNI_MARK" 2>/dev/null || true)
        if [ -n "$_line" ]; then
            HIT_Y=$_py
            HIT_ID=$(printf '%s' "$_line" | sed -n 's/.*id=\([0-9]*\).*/\1/p')
            case "$_line" in
            *"<0>"*) ok "the click sent Event(id,clicked,<int32 0>,ts)" ;;
            *) bad "the clicked data variant was not an int32: $_line" ;;
            esac
            break
        fi
        _py=$((_py + 2))
    done
    HIT_OFF=""
    if [ -n "$HIT_Y" ]; then
        HIT_OFF=$((HIT_Y - PY))
        ok "a press inside the popup hit a row at y=$HIT_Y (id=$HIT_ID)"
        [ "$HIT_ID" = "11" ] && ok "and it was the first normal row, Quit" \
            || bad "the first hit row was id=$HIT_ID, expected 11"
    else
        bad "no press inside the popup activated any row"
    fi
    [ "$(grep -c 'Event clicked' "$SNI_MARK" 2>/dev/null || echo 0)" -eq 1 ] \
        && ok "exactly one clicked event was sent" \
        || bad "more than one clicked event was sent"
    sleep 0.5
    popup_gone && ok "the popup window is gone after the click" \
        || bad "the popup window survived the click"
    grep -q "MENU /TrayAy Event closed" "$SNI_MARK" \
        && ok "the menu was closed on the client too" \
        || bad "no Event(closed) after the click"
fi

echo "-- phase D: a separator or disabled row is inert"
: >"$SNI_MARK"
if open_menu ay "reopened"; then
    PX=$(win_field "$POPUP" x); PY=$(win_field "$POPUP" y)
    PH=$(win_field "$POPUP" h)
    # Walk down the popup one press at a time, reopening between them: an
    # activating press closes the menu, so without the reopen the next
    # probe would be measuring an empty screen. The first press that does
    # nothing at all - and leaves the popup mapped - is a separator or a
    # disabled row, proved by observation rather than by assuming a row
    # height.
    IDLE=0; INERT_PROVEN=no; ACT=0
    # Start just below the band that activated, so the first inert press
    # found is a separator or disabled *row* rather than the title band.
    _py=$((PY + ${HIT_OFF:-4} + 4)); _end=$((PY + PH - 2)); _guard=0
    while [ "$_py" -le "$_end" ] && [ "$_guard" -lt 60 ]; do
        : >"$SNI_MARK"
        press $((PX + 6)) "$_py" 1
        sleep 0.2
        _line=$(grep -m1 "Event clicked" "$SNI_MARK" 2>/dev/null || true)
        if [ -n "$_line" ]; then
            ACT=$((ACT + 1))
        else
            if popup_gone; then
                bad "an inert press closed the menu at y=$_py"
                INERT_PROVEN=bad
                break
            fi
            IDLE=$((IDLE + 1))
            INERT_PROVEN=yes
            break
        fi
        # step past the band that just activated, then wait for the popup
        _py=$((_py + 3))
        if ! reopen_popup; then
            bad "the popup did not come back after an activating press"
            break
        fi
        PX=$(win_field "$POPUP" x); PY=$(win_field "$POPUP" y)
        PH=$(win_field "$POPUP" h)
        _guard=$((_guard + 1))
    done
    if [ "$INERT_PROVEN" = yes ]; then
        ok "a separator or disabled row was hit and did nothing ($IDLE idle press, $ACT activating presses before it)"
        ok "and it left the popup open"
    elif [ "$INERT_PROVEN" = bad ]; then
        :
    else
        bad "no inert band found: every row in the popup activated"
    fi
    # One level only, proven by walking the whole popup. The rows the
    # designer's popup draws are the layout's own children: id 11, the
    # separator, 13, 14 and the disabled 15. So the set of ids a full press
    # sweep reaches must be exactly {11, 13, 14} - a flattened submenu would
    # add 171, a lost separator or disabled-row check would add 12 or 15.
    SWEEP_IDS=""
    _sy=$((PY + 2)); _send=$((PY + PH - 2))
    # The popup's own height is the loop's bound, so it always ends: every
    # iteration moves down, and _send is re-read whenever the popup is
    # reopened because the strip may have moved it.
    while [ "$_sy" -le "$_send" ]; do
        : >"$SNI_MARK"
        press $((PX + 6)) "$_sy" 1
        sleep 0.18
        _line=$(grep -m1 "Event clicked" "$SNI_MARK" 2>/dev/null || true)
        if [ -n "$_line" ]; then
            SWEEP_IDS="$SWEEP_IDS $(printf '%s' "$_line" |
                sed -n 's/.*id=\([0-9]*\).*/\1/p')"
            if ! reopen_popup; then
                bad "the popup did not come back during the row sweep"
                break
            fi
            PX=$(win_field "$POPUP" x); PY=$(win_field "$POPUP" y)
            PH=$(win_field "$POPUP" h)
            _send=$((PY + PH - 2))
        fi
        _sy=$((_sy + 2))
    done
    UNIQUE=$(printf '%s\n' $SWEEP_IDS | sort -u | tr '\n' ' ' | tr -s ' ')
    echo "     rows the sweep reached:$UNIQUE"
    if [ "$UNIQUE" = "11 13 14 " ]; then
        ok "a full press sweep reaches exactly the layout's own rows"
    else
        bad "the press sweep reached [$UNIQUE], expected [11 13 14 ]"
    fi
    : >"$SNI_MARK"
    # An outside press. The bar is the only WM surface here that selects
    # BUTTON_PRESS and is not the popup: the root window selects no
    # BUTTON_PRESS, so a click on empty desktop never reaches this WM at
    # all, and no X client is installed to map a window of its own.
    press $((BX + 40)) "$Y" 1
    sleep 0.8
    popup_gone && ok "an outside press closed the popup" \
        || bad "an outside press did not close the popup"
    if grep -q "SNI " "$SNI_MARK" 2>/dev/null; then
        bad "the outside press reached a tray item: $(head -1 "$SNI_MARK")"
    else
        ok "the outside press reached no tray item"
    fi
    echo "  BLOCKED the swallowed half of an outside press is unproven here:"
    echo "         the root selects no BUTTON_PRESS and no X client is installed," 
    echo "         so no press the WM can see lands on a non-bar, non-popup surface"
fi

close_popup
echo "-- phase E: Escape closes an open popup"
if open_menu ay "opened for Escape"; then
    xdotool key --clearmodifiers Escape 2>/dev/null
    sleep 0.6
    popup_gone && ok "Escape closed the popup" \
        || bad "Escape did not close the popup"
fi

close_popup
echo "-- phase F: a LayoutUpdated refreshes rows while the popup is up"
: >"$SNI_MARK"
if open_menu ay "opened for the refetch"; then
    L0=$(count "MENU /TrayAy GetLayout" "$SNI_MARK")
    sleep 2.6
    L1=$(count "MENU /TrayAy GetLayout" "$SNI_MARK")
    if [ "$L1" -gt "$L0" ]; then
        ok "a LayoutUpdated caused a refetch ($L0 -> $L1 GetLayout calls)"
    else
        bad "a LayoutUpdated did not cause a refetch ($L0 -> $L1)"
    fi
    if popup_gone; then
        bad "the popup closed across the refetch"
    else
        ok "the popup stayed open across the refetch"
    fi
    xdotool key --clearmodifiers Escape 2>/dev/null
    sleep 0.5
fi

close_popup
echo "-- phase G: the split-path item addresses /MenuBar"
: >"$SNI_MARK"
if [ -n "$(cell_of kde)" ]; then
    if open_menu kde "the KDE item"; then
        K=$(count "MENU /MenuBar " "$SNI_MARK")
        S=$(count "MENU /StatusNotifierItem " "$SNI_MARK")
        [ "$K" -gt 0 ] && ok "its menu calls addressed /MenuBar ($K of them)" \
            || bad "no menu call addressed /MenuBar"
        [ "$S" -eq 0 ] && ok "and none addressed the SNI path" \
            || bad "$S menu calls were misaddressed to the SNI path"
        xdotool key --clearmodifiers Escape 2>/dev/null
        sleep 0.5
    fi
else
    bad "no cell for the split-path item"
fi

close_popup
echo "-- phase G2: a sub-path menu object is addressed where it lives"
: >"$SNI_MARK"
if open_menu sub "the sub-path item"; then
    ok "the sub-path item's popup opened"
    K=$(count "MENU /TraySub/Menu " "$SNI_MARK")
    S=$(count "MENU /TraySub " "$SNI_MARK")
    [ "$K" -gt 0 ] && ok "its menu calls addressed /TraySub/Menu ($K of them)" \
        || bad "no menu call addressed /TraySub/Menu"
    [ "$S" -eq 0 ] && ok "and none addressed the item's own object" \
        || bad "$S menu calls were misaddressed to /TraySub"
    xdotool key --clearmodifiers Escape 2>/dev/null
    sleep 0.5
else
    bad "no cell for the sub-path item"
fi

close_popup
echo "-- phase G3: a children property is still understood, as a fallback"
: >"$SNI_MARK"
if open_menu leg "the legacy item"; then
    ok "the compatibility layout still published a popup"
    L=$(count "MENU /TrayLeg GetLayout" "$SNI_MARK")
    [ "$L" -gt 0 ] && ok "its GetLayout was answered ($L of them)" \
        || bad "no GetLayout call reached the legacy item"
    LX=$(win_field "$POPUP" x); LY=$(win_field "$POPUP" y)
    LW=$(win_field "$POPUP" w); LH=$(win_field "$POPUP" h)
    _py=$((LY + 2)); _end=$((LY + LH - 2)); LEG_IDS=""
    while [ "$_py" -le "$_end" ]; do
        : >"$SNI_MARK"
        press $((LX + 6)) "$_py" 1
        sleep 0.16
        _line=$(grep -m1 "Event clicked" "$SNI_MARK" 2>/dev/null || true)
        if [ -n "$_line" ]; then
            LEG_IDS="$LEG_IDS $(printf '%s' "$_line" |
                sed -n 's/.*id=\([0-9]*\).*/\1/p')"
            # an activating press closes the menu, so this is the last one
            break
        fi
        _py=$((_py + 2))
    done
    case " $LEG_IDS " in
    *" 11 "*) ok "its first row is the Quit row, so the rows were read" ;;
    *) bad "the compatibility rows were not read (ids:$LEG_IDS)" ;;
    esac
    sleep 0.4
    popup_gone && ok "the compatibility popup closed on the click" \
        || bad "the compatibility popup survived the click"
else
    bad "no cell for the compatibility item"
fi

close_popup
echo "-- phase H: the themed row icon is drawn, proven by pixel difference"
# The two popups carry identical rows and labels. The Ayatana item asks for
# an icon that exists only inside its IconThemePath; the KDE item asks for
# the same name with no theme path, so it gets nothing. Each capture is
# cropped relative to its own window, so the two screens positions do not
# matter.
if [ "${THEME_OK:-no}" = no ]; then
    echo "  SKIP the themed row icon: no usable icon theme, so the two popups"
    echo "       would be identical for a reason that has nothing to do with"
    echo "       the backend"
else
    : >"$SNI_MARK"
    if open_menu ay "the themed popup captured"; then
        xwd -id "$POPUP" -out "$TMP/ay.xwd" 2>/dev/null
        AW=$(win_field "$POPUP" w); AH=$(win_field "$POPUP" h)
        xdotool key --clearmodifiers Escape 2>/dev/null
        sleep 0.6
    fi
    : >"$SNI_MARK"
    if open_menu kde "the unthemed popup captured"; then
        xwd -id "$POPUP" -out "$TMP/kde.xwd" 2>/dev/null
        KW=$(win_field "$POPUP" w); KH=$(win_field "$POPUP" h)
        xdotool key --clearmodifiers Escape 2>/dev/null
        sleep 0.5
    fi
    if [ -f "$TMP/ay.xwd" ] && [ -f "$TMP/kde.xwd" ]; then
        MW=$AW; [ "$KW" -lt "$MW" ] && MW=$KW
        MH=$AH; [ "$KH" -lt "$MH" ] && MH=$KH
        echo "     themed popup ${AW}x${AH}, unthemed ${KW}x${KH}, comparing ${MW}x${MH}"
        convert "$TMP/ay.xwd" -crop "${MW}x${MH}+0+0" +repage "$TMP/ay.crop" 2>/dev/null
        convert "$TMP/kde.xwd" -crop "${MW}x${MH}+0+0" +repage "$TMP/kde.crop" 2>/dev/null
        D=$(changed_pixels "$TMP/ay.crop" "$TMP/kde.crop")
        if [ -n "$D" ] && [ "$D" -gt 0 ] 2>/dev/null; then
            ok "the popups differ by ${D}px, so the themed row icon is drawn"
        elif [ "$AW" -ne "$KW" ]; then
            ok "the popups differ in width (${AW} vs ${KW}), so the row icon is drawn"
        else
            bad "the two popups are identical, so no row icon was drawn"
        fi
    else
        bad "could not capture both popups"
    fi
fi

close_popup
echo "-- phase I: a broken menu falls back to the item's ContextMenu"
if BC=$(press_item bad 3); then
    sleep 1.5
    E=$(count "MENU /TrayBad ERROR AboutToShow" "$SNI_MARK")
    C=$(count "SNI /TrayBad ContextMenu" "$SNI_MARK")
    G=$(count "MENU /TrayBad GetLayout" "$SNI_MARK")
    [ "$E" -gt 0 ] && ok "the broken menu was asked and errored" \
        || bad "the broken menu was never asked"
    [ "$G" -eq 0 ] && ok "and no GetLayout followed the error" \
        || bad "$G GetLayout calls followed a failed AboutToShow"
    [ "$C" -eq 1 ] && ok "the item's own ContextMenu was sent instead" \
        || bad "$C ContextMenu calls for the broken menu, expected 1"
    [ -z "$(find_popup "$BC")" ] \
        && ok "no popup was left up for the failed menu" \
        || bad "a popup window was left up after the menu failed"
else
    bad "no press on the broken item's cell reached that item"
fi

close_popup
echo "-- phase J: an item with no Menu falls back too"
if NC=$(press_item none 3); then
    sleep 0.8
    C=$(count "SNI /TrayNone ContextMenu" "$SNI_MARK")
    [ "$C" -eq 1 ] && ok "the menu-less item sent its own ContextMenu" \
        || bad "$C ContextMenu calls for the menu-less item at x=$NC, expected 1"
    N=$(count "MENU /TrayNone " "$SNI_MARK")
    [ "$N" -eq 0 ] && ok "and no menu exchange was attempted" \
        || bad "$N menu calls were sent to a menu-less item"
else
    bad "no press on the menu-less item's cell reached that item"
fi

close_popup
echo "-- phase K: the item dying mid-popup destroys the popup"
: >"$SNI_MARK"
if open_menu noic "opened before the item dies"; then
    stop_item noic
    sleep 1.8
    if popup_gone; then
        ok "the popup was destroyed when its item died"
    else
        bad "the popup outlived the item that owned it"
    fi
    if wait_items 6 60; then
        ok "the registry dropped to 6 items"
    else
        bad "the dead item was never unregistered"
    fi
    if grep -q "no answer in time" "$LOG"; then
        bad "the menu deadline fired after the item was gone"
    else
        ok "no menu deadline fired after the item was gone"
    fi
fi

# Open one tray item's own menu without asking any particular item to
# answer quickly. A press to the right of the tray is austere's settings
# menu, so a popup on its own is not proof of a tray exchange: the item's
# own AboutToShow in the mark is. Both waits are generous on purpose,
# because the whole point of the phase is a wm that is (or is not) busy,
# and a check that only works on a fast one proves nothing.
open_a_tray_menu() {
    _px=$((BX + BW - 6))
    while [ "$_px" -gt $((BX + BW * 30 / 100)) ]; do
        _p=""
        : >"$SNI_MARK"
        press "$_px" "$Y" 3
        _t=0
        while [ "$_t" -lt 30 ]; do
            _p=$(find_popup "$_px")
            [ -n "$_p" ] && { POPUP="$_p"; CELL="$_px"; break; }
            sleep 0.1
            _t=$((_t + 1))
        done
        if [ -n "$_p" ]; then
            _t=0
            while [ "$_t" -lt 20 ]; do
                grep -q "MENU .*AboutToShow" "$SNI_MARK" 2>/dev/null &&
                    { echo "$_px"; return 0; }
                sleep 0.1
                _t=$((_t + 1))
            done
            xdotool key --clearmodifiers Escape
            sleep 0.3
        fi
        _px=$((_px - 6))
    done
    return 1
}

echo "-- phase K2: a dismissed menu leaves the loop idle, not spinning"
# The tray arms one wakeup for a menu exchange, and that wakeup expires on
# its own: a menu that was answered, dismissed or failed leaves nothing
# behind to wait for. The evidence here is deliberately not a pixel diff,
# because a repaint of identical content is invisible - a repaint storm
# has nothing to show on the screen it is thrashing. What it does have is
# cost: a wakeup that is never cleared answers poll() with a timeout of
# zero, forever, and every one of those turns draws a full bar and makes
# the X server push all of those pixels again.
#
# So this measures what the two processes burn while nothing happens, over
# windows that sit well past the wakeup the press armed (2000 ms). A loop
# that sleeps reads 0-1 for the wm and 0 for the server; a stuck deadline
# reads around half a core for the wm (a tenth of one where the bar has
# nothing else to draw) and a few percent for the server, for as long as
# you watch. Both numbers come from /proc, so a host without it reports
# SKIP for the phase instead of passing it vacuously.
cpu_jiffies() {   # cpu_jiffies <pid> -> utime+stime in clock ticks
    awk '{ print $14 + $15 }' "/proc/$1/stat" 2>/dev/null || echo ""
}
if [ ! -r "/proc/$WM_PID/stat" ] || [ ! -r "/proc/$XVFB_PID/stat" ]; then
    echo "  SKIP no /proc cpu accounting: the idle-repaint check needs it"
else
    TICKS=$(getconf CLK_TCK 2>/dev/null || echo 100)
    # A press, an exchange and a dismissal: the sequence that arms the
    # wakeup, so what follows measures a spent one.
    if _cell=$(open_a_tray_menu) && [ -n "$_cell" ]; then
        ok "a tray item's own menu opened for the idle check (cell x=$_cell)"
        close_popup
        sleep 2.5
        w0=$(cpu_jiffies "$WM_PID")
        x0=$(cpu_jiffies "$XVFB_PID")
        sleep 3
        w1=$(cpu_jiffies "$WM_PID")
        x1=$(cpu_jiffies "$XVFB_PID")
        wj=$((w1 - w0))
        xj=$((x1 - x0))
        # Printed, not asserted: the server's own cost is the same drawing
        # seen from the other side, and it moves with how much the bar has
        # to draw rather than with the loop's own behaviour. The wm's
        # number is the assertion - a stuck deadline measures 0.5-1.0 of a
        # core here, a loop that sleeps measures 0, and the threshold sits
        # an order of magnitude above healthy.
        echo "     3s idle: wm $wj jiffies, x server $xj jiffies (CLK_TCK=$TICKS)"
        if [ "$wj" -lt $((TICKS * 3 / 5)) ]; then
            ok "the wm sleeps once the menu's wakeup is spent ($wj/$TICKS s)"
        else
            bad "the wm is still busy 3s after the menu closed ($wj/$TICKS s)"
        fi
        # and it must not creep back: an overdue deadline serviced once,
        # rather than pinned, is the difference this second window proves
        sleep 3
        w2=$(cpu_jiffies "$WM_PID")
        wk=$((w2 - w1))
        if [ "$wk" -lt $((TICKS * 3 / 5)) ]; then
            ok "the 3s after that are idle as well ($wk/$TICKS s)"
        else
            bad "the wm is busy again in the window after that ($wk/$TICKS s)"
        fi
    fi
fi

echo "-- phase L: shutdown is clean"
# The canonical layout must have produced rows for every menu in this
# harness, so an empty-layout fallback is a failure here and not only in
# the real-app acceptance: a parser that only understood the fabricated
# "children" property would sail past every earlier phase otherwise.
if grep -q "no rows to show" "$LOG"; then
    bad "a GetLayout answer produced no rows at all:"
    grep -m3 "no rows to show" "$LOG"
else
    ok "no GetLayout answer was rejected as empty"
fi
for t in ay kde bad none sub leg; do
    stop_item "$t"
done
kill -TERM "$WM_PID" 2>/dev/null
i=0
while [ "$i" -lt 100 ]; do
    kill -0 "$WM_PID" 2>/dev/null || break
    sleep 0.1
    i=$((i + 1))
done
if kill -0 "$WM_PID" 2>/dev/null; then
    bad "the wm ignored SIGTERM"
    kill -9 "$WM_PID" 2>/dev/null
else
    ok "the wm exited on SIGTERM"
fi
wait "$WM_PID" 2>/dev/null || true
WM_PID=""
if grep -qiE "assertion|segmentation|double free|corrupt|free\(\): " "$LOG"; then
    bad "the log reports a memory or protocol fault:"
    grep -iE "assertion|segmentation|double free|corrupt|free\(\): " "$LOG" | head -3
else
    ok "the log has no assertion, leak or corruption report"
fi

echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
CHECK

export WM_PID LOG="$TMP/austere.log" SNI_MARK="$TMP/mark" DISPLAY="$DISP"
# The idle-repaint phase needs to know which process is the X server, to
# watch it for drawing it was never asked to do.
export XVFB_PID
export BIN TMP
export HOME="$TMP/home"
export XDG_CONFIG_HOME="$TMP/home/.config"
export XDG_DATA_HOME="$TMP/home/.local/share"
export XDG_CACHE_HOME="$TMP/home/.cache"
export XDG_RUNTIME_DIR="$TMP/home/run"
export PROBE_THEME="${PROBE_THEME:-/usr/share/icons/clarity-luteus}"
# The theme-dependent phase needs a real theme directory: the icon the
# first row asks for exists only inside it, and its absence is the whole
# evidence for the theme resolver. Where there is none, the fakes are told
# about no theme at all and that phase reports SKIP rather than failing
# every run of the suite on a machine without this particular theme.
THEME_OK=yes
if [ ! -d "$PROBE_THEME" ] || [ ! -f "$PROBE_THEME/index.theme" ]; then
    THEME_OK=no
    echo "SKIP: no usable icon theme at $PROBE_THEME; the themed row icon"
    echo "      phase will report SKIP and the rest of the suite still runs"
    PROBE_THEME=""
fi
export THEME_OK
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
    "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

if dbus-run-session -- sh "$TMP/check.sh"; then
    echo "PASS: tray menu acceptance"
    RC=0
else
    RC=$?
    echo "FAIL: tray menu acceptance (rc=$RC)" >&2
    [ -f "$TMP/austere.log" ] && sed -n '1,80p' "$TMP/austere.log" >&2
fi
exit $RC

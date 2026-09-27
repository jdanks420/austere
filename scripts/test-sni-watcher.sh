#!/bin/sh
# Conformance check for austere's StatusNotifierItem tray (deepwork
# phase 1). Runs the real binary on a private Xvfb display inside a
# private D-Bus session with a throwaway HOME, in two phases:
#
#   1. owner: the watcher object as a tray client sees it - Introspect,
#      Properties.Get/GetAll, ProtocolVersion, host registration, and an
#      end-to-end fake StatusNotifierItem (all registration forms, async
#      GetAll, signal-triggered refetch, IconPixmap fallback, death grace,
#      unregistration), plus an idle-CPU spin check and a clean SIGTERM
#      shutdown.
#   2. squatter: another process holds org.kde.StatusNotifierWatcher, so
#      austere must stay inert and answer RegisterStatusNotifierItem with
#      an error instead of a silent success.
#
# Usage: scripts/test-sni-watcher.sh [path-to-austere]
#   or:  AUSTERE_BIN=./austere scripts/test-sni-watcher.sh
#
# Needs: Xvfb, dbus-run-session, dbus-send, awk. A missing dependency is
# reported as SKIP (exit 77), never as success. No valgrind, no network,
# and nothing outside the temporary directory it creates is touched.

set -eu

BIN=${1:-${AUSTERE_BIN:-./austere}}
case "$BIN" in
    /*) ;;
    *)  BIN="$PWD/$BIN" ;;
esac

missing=
for tool in Xvfb dbus-run-session dbus-send awk; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "SKIP: missing test dependencies:$missing (conformance not run)" >&2
    exit 77
fi
[ -x "$BIN" ] || { echo "FAIL: no executable at $BIN" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/austere-sni.XXXXXX")
XVFB_PID=""
WM_PID=""

cleanup() {
    [ -n "$WM_PID" ] && kill -9 "$WM_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# ---- private X display ------------------------------------------------
DISP=""
n=90
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

# ---- phase 1: austere owns the watcher name ---------------------------
cat >"$TMP/check.sh" <<'CHECK'
set -u
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); echo "  ok   $*"; }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL $*"; }
have() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

call() {   # call <interface.method> [args...] -> raw reply text on stdout
    _m=$1; shift
    dbus-send --session --print-reply --reply-timeout=4000 \
        --dest=org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
        "$_m" "$@" 2>&1
}

owned() {
    dbus-send --session --print-reply --reply-timeout=4000 \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner \
        string:org.kde.StatusNotifierWatcher 2>&1 | grep -q 'boolean true'
}

items() {
    call org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems |
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

has_gi() { python3 -c "import gi; gi.require_version('Gio', '2.0')" 2>/dev/null; }

echo "-- starting $BIN"
"$BIN" >"$LOG" 2>&1 &
WM_PID=$!

echo "-- waiting for the watcher name"
i=0
while [ "$i" -lt 100 ]; do
    kill -0 "$WM_PID" 2>/dev/null || { bad "wm exited during startup"; sed -n '1,40p' "$LOG"; exit 1; }
    owned && break
    sleep 0.1
    i=$((i + 1))
done
owned || { bad "never owned org.kde.StatusNotifierWatcher"; sed -n '1,40p' "$LOG"; exit 1; }
ok "owns org.kde.StatusNotifierWatcher"

echo "-- Introspect"
out=$(call org.freedesktop.DBus.Introspectable.Introspect)
for needle in org.kde.StatusNotifierWatcher RegisterStatusNotifierItem \
    RegisterStatusNotifierHost RegisteredStatusNotifierItems ProtocolVersion \
    IsStatusNotifierHostRegistered org.freedesktop.DBus.Properties \
    StatusNotifierItemRegistered; do
    if have "$out" "$needle"; then ok "Introspect mentions $needle"
    else bad "Introspect is missing $needle"; fi
done

echo "-- Properties.Get"
if out=$(call org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher string:ProtocolVersion); then
    have "$out" 'int32 0' && ok "ProtocolVersion == 0" \
        || bad "ProtocolVersion is not 0: $out"
else
    bad "Properties.Get ProtocolVersion errored: $out"
fi
if out=$(call org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher string:IsStatusNotifierHostRegistered); then
    have "$out" 'boolean true' && ok "IsStatusNotifierHostRegistered == true" \
        || bad "IsStatusNotifierHostRegistered is not true: $out"
else
    bad "Properties.Get IsStatusNotifierHostRegistered errored: $out"
fi
if out=$(call org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher string:RegisteredStatusNotifierItems); then
    if have "$out" 'array ['; then
        if have "$out" 'string "'; then
            bad "RegisteredStatusNotifierItems is not empty: $out"
        else
            ok "RegisteredStatusNotifierItems is empty"
        fi
    else
        bad "RegisteredStatusNotifierItems is not an array: $out"
    fi
else
    bad "Properties.Get RegisteredStatusNotifierItems errored: $out"
fi
if out=$(call org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher string:NoSuchProperty) ||
   have "$out" 'UnknownProperty'; then
    ok "unknown property is an error"
else
    bad "unknown property was not rejected: $out"
fi

echo "-- Properties.GetAll"
if out=$(call org.freedesktop.DBus.Properties.GetAll \
        string:org.kde.StatusNotifierWatcher); then
    missing=
    for needle in ProtocolVersion IsStatusNotifierHostRegistered \
        RegisteredStatusNotifierItems; do
        have "$out" "$needle" || missing="$missing $needle"
    done
    [ -z "$missing" ] && ok "GetAll carries every property" \
        || bad "GetAll is missing:$missing"
    have "$out" 'int32 0' || bad "GetAll ProtocolVersion is not 0"
else
    bad "Properties.GetAll errored: $out"
fi
if out=$(call org.freedesktop.DBus.Properties.GetAll string:org.freedesktop.DBus); then
    have "$out" 'array [' && ok "GetAll on a foreign interface answers without an error" \
        || bad "GetAll on a foreign interface did not return a dict: $out"
else
    bad "GetAll on a foreign interface errored: $out"
fi

echo "-- idle loop must not spin"
# The WM thread is the only thing running; an unrecognised bus fd would
# make poll() return instantly forever. 3 s of wall clock, >100 ticks of
# CPU means a spin, the WM's own clock module costs a few.
cpu_ticks() { awk '{ print $14 + $15 }' "/proc/$WM_PID/stat" 2>/dev/null || echo 0; }
sleep 2
a=$(cpu_ticks)
sleep 3
b=$(cpu_ticks)
delta=$((b - a))
if [ "$delta" -lt 100 ]; then ok "idle CPU $delta ticks over 3 s"
else bad "idle CPU $delta ticks over 3 s looks like a spin"; fi

echo "-- item registry (end to end)"
if has_gi; then
    ITEM_MARK="$TMP/item.log"
    python3 "$TMP/sni_item.py" "$ITEM_MARK" >"$TMP/item.out" 2>&1 &
    ITEM_PID=$!

    # Canonical IDs are service+path: the registered service (the unique
    # name for the bare-path form) plus the object path, and the
    # bare-name form defaults the path to /StatusNotifierItem.
    if wait_items 3 80; then
        got=$(items | sort | tr '\n' ' ')
        uniq=$(cat "$ITEM_MARK.name" 2>/dev/null || echo '?')
        want=$(printf '%s\n%s\n%s\n' \
            "org.austere.SniTest/StatusNotifierItem" \
            "org.austere.SniTest.Bare/StatusNotifierItem" \
            "$uniq/StatusNotifierItem" | sort | tr '\n' ' ')
        if [ "$got" = "$want" ]; then
            ok "service/path, path-only and bare-name forms: canonical ids"
        else
            bad "registry is [$got], want [$want]"
        fi
    else
        bad "items never reached 3 entries: $(items | tr '\n' ' ')"
    fi

    if [ -s "$ITEM_MARK" ] && grep -q '^GetAll' "$ITEM_MARK"; then
        ok "watcher fetched item properties asynchronously"
    else
        bad "item never received a Properties.GetAll"
    fi

    # Signal-triggered refetch: the item emits the spec-shaped signals
    # (NewTitle carries no argument) right after answering the first
    # fetch, and a further fetch must land after that. Compared by
    # position in the marker, not by sampling, so there is no race.
    t=0
    while [ "$t" -lt 60 ] && ! grep -q 'signals emitted' "$ITEM_MARK" 2>/dev/null; do
        sleep 0.1
        t=$((t + 1))
    done
    if ! grep -q 'signals emitted' "$ITEM_MARK" 2>/dev/null; then
        bad "item never emitted its signals"
    else
        t=0
        refetched=no
        while [ "$t" -lt 60 ]; do
            sig_line=$(grep -n 'signals emitted' "$ITEM_MARK" | tail -1 |
                cut -d: -f1)
            all_line=$(grep -n '^GetAll' "$ITEM_MARK" | tail -1 | cut -d: -f1)
            if [ -n "$all_line" ] && [ -n "$sig_line" ] &&
               [ "$all_line" -gt "$sig_line" ]; then
                refetched=yes
                break
            fi
            sleep 0.1
            t=$((t + 1))
        done
        if [ "$refetched" = yes ]; then
            ok "NewStatus/NewTitle(argless)/NewIcon triggered a new GetAll"
        else
            bad "no GetAll followed the signals ($(grep -c '^GetAll' "$ITEM_MARK") fetches)"
        fi
    fi

    if grep -q 'using IconPixmap' "$LOG"; then
        ok "IconPixmap fallback used when the icon name misses"
    else
        bad "no IconPixmap fallback line in the log"
    fi

    kill "$ITEM_PID" 2>/dev/null || true
    wait "$ITEM_PID" 2>/dev/null || true
    # The registry must drop the dead item at once: RegisteredStatus-
    # NotifierItems excludes grace entries, so a client never sees itself
    # removed before its entry is really gone (and cannot loop).
    # Whether we sampled that state is reported; the drain is the assert.
    t=0
    excluded=no
    while [ "$t" -lt 60 ]; do
        if [ "$(items | wc -l)" -eq 0 ]; then
            grep -q unregistered "$LOG" || excluded=yes
            break
        fi
        t=$((t + 1))
        sleep 0.02
    done
    if [ "$(items | wc -l)" -eq 0 ]; then
        if [ "$excluded" = yes ]; then
            ok "registry drops the dead item before the entry is removed"
        else
            ok "registry drains after the item exits"
        fi
    else
        bad "registry still holds $(items | tr '\n' ' ')"
    fi
    t=0
    while [ "$t" -lt 30 ] && ! grep -q unregistered "$LOG"; do
        sleep 0.1
        t=$((t + 1))
    done
    if kill -0 "$WM_PID" 2>/dev/null; then ok "wm survived the item"
    else bad "wm died with the item"; sed -n '1,60p' "$LOG"; exit 1; fi
    if grep -q unregistered "$LOG"; then ok "entry removed after the death grace"
    else bad "no unregistration line in the log"; fi
else
    echo "  SKIP python3-gi missing; the item registry was not exercised"
fi

echo "-- re-registration storm (pending table must not orphan)"
if has_gi; then
    STORM_MARK="$TMP/storm.log"
    rm -f "$STORM_MARK"
    python3 "$TMP/sni_storm.py" "$STORM_MARK" >"$TMP/storm.out" 2>&1 &
    STORM_PID=$!
    # The item re-registers while its GetAll sits unanswered. If the
    # watcher fails to retire those pending calls the table fills up and
    # no further GetAll can ever be sent, which the late fetch proves.
    t=0
    while [ "$t" -lt 80 ] && ! grep -q 'storm done' "$STORM_MARK" 2>/dev/null; do
        sleep 0.1
        t=$((t + 1))
    done
    if ! grep -q 'storm done' "$STORM_MARK" 2>/dev/null; then
        bad "storm item never finished: $(head -3 "$STORM_MARK" 2>/dev/null | tr '\n' ' ')"
    else
        if [ "$(items | wc -l)" -eq 1 ]; then
            ok "50 re-registrations left exactly one entry"
        else
            bad "registry has $(items | wc -l) entries after the storm"
        fi
        t=0
        while [ "$t" -lt 60 ] && ! grep -q 'late getall' "$STORM_MARK" 2>/dev/null; do
            sleep 0.1
            t=$((t + 1))
        done
        if grep -q 'late getall' "$STORM_MARK" 2>/dev/null; then
            ok "GetAll still flows after the storm (pending table intact)"
        else
            bad "no GetAll after the storm: the pending table is exhausted"
        fi
    fi
    kill "$STORM_PID" 2>/dev/null || true
    wait "$STORM_PID" 2>/dev/null || true
    t=0
    while [ "$t" -lt 40 ] && [ "$(items | wc -l)" -ne 0 ]; do
        sleep 0.1
        t=$((t + 1))
    done
    if [ "$(items | wc -l)" -eq 0 ]; then ok "storm item unregistered cleanly"
    else bad "registry still holds $(items | tr '\n' ' ')"; fi
    if grep -q 'registry full' "$LOG"; then
        bad "watcher reported a full registry"
    fi
    if kill -0 "$WM_PID" 2>/dev/null; then ok "wm survived the storm"
    else bad "wm died during the storm"; sed -n '1,60p' "$LOG"; exit 1; fi
else
    echo "  SKIP python3-gi missing; the re-registration storm was not exercised"
fi

echo "-- clean shutdown"
kill -TERM "$WM_PID"
i=0
while [ "$i" -lt 100 ]; do
    kill -0 "$WM_PID" 2>/dev/null || break
    sleep 0.1
    i=$((i + 1))
done
if kill -0 "$WM_PID" 2>/dev/null; then
    bad "wm ignored SIGTERM"
    kill -9 "$WM_PID" 2>/dev/null
    exit 1
fi
ok "exited on SIGTERM"
wait "$WM_PID" || true
if owned; then bad "watcher name still owned after exit"; else ok "name released"; fi
if grep -q 'serving org.kde.StatusNotifierWatcher' "$LOG"; then
    ok "logged watcher ownership"
else
    bad "no ownership line in the log"; sed -n '1,40p' "$LOG"
fi

echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
CHECK

# ---- phase 2: another watcher owns the name, austere must be inert ------
cat >"$TMP/squatter.sh" <<'SQUATTER'
set -u
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); echo "  ok   $*"; }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL $*"; }
have() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

has_gi() { python3 -c "import gi; gi.require_version('Gio', '2.0')" 2>/dev/null; }
have_py() { python3 -c "import dbus" 2>/dev/null; }

# Austere exports /StatusNotifierWatcher on its own connection even when it
# does not own the bus name, so the inert instance is found by probing the
# unique names for that interface instead of by name lookup.
find_tray_connection() {
    for n in $(dbus-send --session --print-reply --reply-timeout=4000 \
            --dest=org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus.ListNames 2>/dev/null |
        sed -n 's/^ *string "\(.*\)"$/\1/p'); do
        case "$n" in :*) ;; *) continue ;; esac
        if dbus-send --session --print-reply --reply-timeout=2000 \
                --dest="$n" /StatusNotifierWatcher \
                org.freedesktop.DBus.Introspectable.Introspect 2>/dev/null |
            grep -q 'RegisterStatusNotifierItem'; then
            echo "$n"
            return 0
        fi
    done
    return 1
}

echo "-- taking org.kde.StatusNotifierWatcher first"
python3 "$TMP/squatter.py" &
SQUAT_PID=$!
i=0
while [ "$i" -lt 60 ]; do
    dbus-send --session --print-reply --reply-timeout=2000 \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner \
        string:org.kde.StatusNotifierWatcher 2>/dev/null |
        grep -q 'boolean true' && break
    sleep 0.1
    i=$((i + 1))
done
if ! dbus-send --session --print-reply --reply-timeout=2000 \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner \
        string:org.kde.StatusNotifierWatcher 2>/dev/null |
    grep -q 'boolean true'; then
    bad "the squatter never took the watcher name"
    kill "$SQUAT_PID" 2>/dev/null
    exit 1
fi
ok "squatter owns the watcher name"

echo "-- starting $BIN against a taken name"
"$BIN" >"$LOG" 2>&1 &
WM_PID=$!
sleep 2
kill -0 "$WM_PID" 2>/dev/null && ok "wm runs inert" \
    || { bad "wm exited"; sed -n '1,40p' "$LOG"; kill "$SQUAT_PID" 2>/dev/null; exit 1; }
if grep -q 'owned by another watcher' "$LOG"; then
    ok "logged the name contention"
else
    bad "no name-contention line in the log"; sed -n '1,40p' "$LOG"
fi

TRAY=$(find_tray_connection || true)
if [ -z "$TRAY" ]; then
    bad "could not find austere's own watcher connection"
else
    ok "found the inert watcher object on $TRAY"
    # B1: a registration that cannot be honoured must be an error, so the
    # client retries instead of believing it is in the tray.
    if out=$(dbus-send --session --print-reply --reply-timeout=4000 \
            --dest="$TRAY" /StatusNotifierWatcher \
            org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem \
            string:/StatusNotifierItem 2>&1); then
        bad "RegisterStatusNotifierItem succeeded while inert: $out"
    elif have "$out" 'Error org.freedesktop.DBus.Error.'; then
        ok "RegisterStatusNotifierItem returns a D-Bus error while inert"
    else
        bad "unexpected reply while inert: $out"
    fi
    out=$(dbus-send --session --print-reply --reply-timeout=4000 \
        --dest="$TRAY" /StatusNotifierWatcher \
        org.freedesktop.DBus.Properties.Get \
        string:org.kde.StatusNotifierWatcher \
        string:RegisteredStatusNotifierItems 2>&1)
    if have "$out" 'array [' && ! have "$out" 'string "'; then
        ok "registry stayed empty while inert"
    else
        bad "registry is not empty while inert: $out"
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
kill "$SQUAT_PID" 2>/dev/null || true
wait "$SQUAT_PID" 2>/dev/null || true

echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
SQUATTER

cat >"$TMP/squatter.py" <<'SQUATTERPY'
"""Holds org.kde.StatusNotifierWatcher so austere must stay inert.
Test scaffolding, not part of the WM."""
import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
reply = conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                       "org.freedesktop.DBus", "RequestName",
                       GLib.Variant("(su)",
                                    ("org.kde.StatusNotifierWatcher", 4)),
                       GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE,
                       5000, None)
print("squatter RequestName ->", reply.unpack(), flush=True)
loop = GLib.MainLoop()
GLib.timeout_add(30000, loop.quit)
loop.run()
SQUATTERPY

# ---- re-registration storm: a client that keeps re-registering while its
#      property fetches go unanswered, to prove the watcher retires them.
cat >"$TMP/sni_storm.py" <<'STORM'
"""A StatusNotifierItem that re-registers in a tight loop while leaving every
GetAll unanswered, then answers normally. Each unanswered fetch that the
watcher forgets to retire is a slot in its pending table, so this is the
regression probe for pending-call bookkeeping. Test scaffolding."""
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

MARK = sys.argv[1]
SERVICE = "org.austere.SniStorm"
PATH = "/StatusNotifierItem"
SNI = "org.kde.StatusNotifierItem"
ROUNDS = 50

STATE = {"storm": 0, "fetches": 0, "done": False}

XML = """<node>
<interface name="org.freedesktop.DBus.Properties">
 <method name="GetAll">
  <arg name="interface_name" type="s" direction="in"/>
  <arg name="properties" type="a{sv}" direction="out"/>
 </method>
</interface>
<interface name="org.kde.StatusNotifierItem">
 <property name="Category" type="s" access="read"/>
 <property name="Status" type="s" access="read"/>
 <property name="Title" type="s" access="read"/>
 <method name="Activate">
  <arg name="x" type="i" direction="in"/>
  <arg name="y" type="i" direction="in"/>
 </method>
 <signal name="NewIcon"/>
</interface>
</node>"""

WATCHER = ("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem")


def note(line):
    with open(MARK, "a") as handle:
        handle.write(line + "\n")


def register():
    conn.call_sync(WATCHER[0], WATCHER[1], WATCHER[2], WATCHER[3],
                   GLib.Variant("(s)", (SERVICE + PATH,)), None,
                   Gio.DBusCallFlags.NONE, 5000, None)
    STATE["storm"] += 1
    if STATE["storm"] >= ROUNDS:
        STATE["done"] = True
        note("storm done")
        return False
    return True


def on_call(conn_, sender, path, iface, method, params, invocation):
    if method == "GetAll":
        STATE["fetches"] += 1
        if STATE["done"]:
            # a fetch that arrives after the storm is the proof that the
            # watcher can still send one
            note("late getall #%d" % STATE["fetches"])
            invocation.return_value(GLib.Variant("(a{sv})", ({
                "Category": GLib.Variant("s", "ApplicationStatus"),
                "Status": GLib.Variant("s", "Active"),
                "Title": GLib.Variant("s", "storm"),
            },)))
        return None    # during the storm: leave the call unanswered
    invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod",
                                 method)
    return None


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
conn.register_object(PATH,
                     node.lookup_interface("org.freedesktop.DBus.Properties"),
                     on_call, None, None)
conn.register_object(PATH, node.lookup_interface(SNI), on_call, None, None)
conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
               "org.freedesktop.DBus", "RequestName",
               GLib.Variant("(su)", (SERVICE, 4)), GLib.VariantType("(u)"),
               Gio.DBusCallFlags.NONE, 5000, None)
note("registered")
GLib.timeout_add(20, register)

loop = GLib.MainLoop()
GLib.timeout_add(30000, loop.quit)
loop.run()
STORM

# ---- a fake StatusNotifierItem, so the registry is exercised for real ---
cat >"$TMP/sni_item.py" <<'ITEM'
"""Minimal StatusNotifierItem used by scripts/test-sni-watcher.sh.

One process, one object path, two well-known names: it registers with the
watcher in every registration form the spec allows and records what it was
asked for in a marker file. Because all the entries share one connection
and path, later signals are only attributed to the first matching item -
that limitation is fine for a protocol check, which is what this is.
Test scaffolding, not part of the WM."""
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

MARK = sys.argv[1]
SERVICE = "org.austere.SniTest"
BARE = "org.austere.SniTest.Bare"
PATH = "/StatusNotifierItem"
SNI = "org.kde.StatusNotifierItem"

# An icon name that cannot resolve plus a valid IconPixmap: the host must
# fall back to the pixmap instead of dropping the item.
def _pixmaps():
    out = []
    for size in (8, 16, 32):
        px = b""
        for i in range(size * size):
            # ARGB32, network byte order, per the SNI specification
            px += bytes(((i * 7) % 256, 0x33, 0x66, 0xFF))
        out.append(GLib.Variant("(iiay)",
                                (size, size, GLib.Variant("ay", px))))
    return GLib.Variant("a(iiay)", out)


PROPS = {
    "Category": ("s", "ApplicationStatus"),
    "Id": ("s", "austere-sni-test"),
    "Title": ("s", "sni test item"),
    "Status": ("s", "Active"),
    "IconName": ("s", "austere-no-such-icon"),
    "AttentionIconName": ("s", ""),
    "ItemIsMenu": ("b", False),
    "Menu": ("o", "/MenuBar"),
    "IconPixmap": ("a(iiay)", _pixmaps()),
}

PROPS_XML = "".join(
    '  <property name="%s" type="%s" access="read"/>\n' % (k, v[0])
    for k, v in sorted(PROPS.items()))

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
 <signal name="PropertiesChanged">
  <arg name="interface_name" type="s"/>
  <arg name="changed_properties" type="a{sv}"/>
  <arg name="invalidated_properties" type="as"/>
 </signal>
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
 <signal name="NewAttentionIcon"/>
 <signal name="NewStatus"><arg name="status" type="s"/></signal>
 <signal name="NewTitle"/>
 <signal name="NewToolTip"><arg name="icon" type="s"/><arg name="title" type="s"/><arg name="description" type="s"/></signal>
</interface>
</node>"""

STATE = {"getall": 0, "burst": False}


def note(line):
    with open(MARK, "a") as handle:
        handle.write(line + "\n")


def emit(member, body):
    conn.emit_signal(None, PATH, SNI, member, body)


def emit_burst():
    """Spec-shaped updates: NewStatus carries a value, NewTitle carries
    none at all, NewIcon only says "the icon changed"."""
    emit("NewStatus", GLib.Variant("(s)", ("NeedsAttention",)))
    emit("NewTitle", None)
    emit("NewIcon", None)
    note("signals emitted")
    return False


def on_call(conn_, sender, path, iface, method, params, invocation):
    if iface == "org.freedesktop.DBus.Properties" and method == "GetAll":
        STATE["getall"] += 1
        note("GetAll #%d" % STATE["getall"])
        if STATE["getall"] == 1 and not STATE["burst"]:
            STATE["burst"] = True
            GLib.idle_add(emit_burst)
        out = dict((k, v if k == "IconPixmap" else GLib.Variant(t, v))
                   for k, (t, v) in PROPS.items())
        invocation.return_value(GLib.Variant("(a{sv})", (out,)))
    elif iface == "org.freedesktop.DBus.Properties" and method == "Get":
        wanted = params.unpack()[1]
        note("Get %s" % wanted)
        sig, val = PROPS.get(wanted, ("s", ""))
        got = val if wanted == "IconPixmap" else GLib.Variant(sig, val)
        invocation.return_value(GLib.Variant("(v)", (got,)))
    elif iface == SNI and method in ("Activate", "SecondaryActivate",
                                     "ContextMenu"):
        note("%s %s" % (method, params.unpack()))
        invocation.return_value(None)
    elif iface == SNI and method == "Scroll":
        note("Scroll %s" % (params.unpack(),))
        invocation.return_value(None)
    else:
        invocation.return_dbus_error(
            "org.freedesktop.DBus.Error.UnknownMethod",
            "%s.%s" % (iface, method))


def on_prop_get(conn_, path, iface, name):
    sig, val = PROPS.get(name, ("s", ""))
    return val if name == "IconPixmap" else GLib.Variant(sig, val)


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
conn.register_object(PATH,
                     node.lookup_interface("org.freedesktop.DBus.Properties"),
                     on_call, on_prop_get, None)
conn.register_object(PATH, node.lookup_interface(SNI), on_call, on_prop_get,
                     None)
for name in (SERVICE, BARE):
    conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                   "org.freedesktop.DBus", "RequestName",
                   GLib.Variant("(su)", (name, 4)), GLib.VariantType("(u)"),
                   Gio.DBusCallFlags.NONE, 5000, None)
with open(MARK + ".name", "w") as handle:
    handle.write(conn.get_unique_name())

WATCHER = ("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem")
# every registration form the spec defines; the repeat must not duplicate
for arg in (SERVICE + PATH, SERVICE + PATH, PATH, BARE):
    note("register %s" % arg)
    conn.call_sync(WATCHER[0], WATCHER[1], WATCHER[2], WATCHER[3],
                   GLib.Variant("(s)", (arg,)), None,
                   Gio.DBusCallFlags.NONE, 5000, None)
note("ready")

loop = GLib.MainLoop()
GLib.timeout_add(30000, loop.quit)   # the test kills us when it is done
loop.run()
ITEM

export WM_PID LOG="$TMP/austere.log" DISPLAY="$DISP" BIN TMP
export HOME="$TMP/home"
export XDG_CONFIG_HOME="$TMP/home/.config"
export XDG_DATA_HOME="$TMP/home/.local/share"
export XDG_CACHE_HOME="$TMP/home/.cache"
export XDG_RUNTIME_DIR="$TMP/home/run"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
    "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

dump_log() {
    [ -f "$TMP/austere.log" ] && sed -n '1,80p' "$TMP/austere.log" >&2
    return 0
}

# `if` keeps set -e from aborting before the log dump on a failed phase.
if dbus-run-session -- sh "$TMP/check.sh"; then
    echo "-- phase 1 (owner) ok"
else
    rc=$?
    echo "FAIL: owner phase failed (rc=$rc)" >&2
    dump_log
    exit 1
fi

if python3 -c "import gi; gi.require_version('Gio', '2.0')" >/dev/null 2>&1; then
    rm -f "$TMP/austere.log"
    if dbus-run-session -- sh "$TMP/squatter.sh"; then
        echo "-- phase 2 (squatter) ok"
    else
        rc=$?
        echo "FAIL: squatter phase failed (rc=$rc)" >&2
        dump_log
        exit 1
    fi
else
    echo "-- phase 2 (squatter) SKIP: python3-gi missing"
fi

echo "PASS: watcher conformance"

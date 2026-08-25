#!/bin/sh
# Multi-monitor exercise rig for austere (M5).
#
# Xephyr's RandR implementation ignores software monitors (xrandr
# --setmonitor succeeds but GetMonitors never changes), so topology
# changes are injected through the _AUSTERE_TEST_MONITORS ClientMessage
# seam handled by contrib/monpoke — the same code path real RandR events
# reach via monitors_refresh().
#
# Usage: scripts/multimon.sh   (DISPLAY must point at the test display)

DISP=${DISPLAY:-:2}
export DISPLAY=$DISP

case "$DISP" in
    :0|:0.0|:1|:1.0)
        echo "multimon: refusing unsafe DISPLAY '$DISP'" >&2
        exit 2;;
esac

here=$(cd "$(dirname "$0")/.." && pwd)
poke="$here/contrib/monpoke"
fx="$here/contrib/testclient"

pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
nope() { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; }

[ -x "$poke" ] || { echo "multimon: run make tools first" >&2; exit 2; }
xdpyinfo >/dev/null 2>&1 || { echo "multimon: no display $DISP" >&2; exit 2; }

width_of() {
    xwininfo -id "$1" -stats 2>/dev/null |
        awk '/^ *Width:/ { print $2 }'
}
w_lt() { [ "$(width_of "$1")" -lt "$2" ]; }
w_ge() { [ "$(width_of "$1")" -ge "$2" ]; }
desktop_is() {
    [ "$(xprop -root _NET_CURRENT_DESKTOP 2>/dev/null |
        grep -o '[0-9]*')" = "$1" ]
}
alive() { pgrep -x austere >/dev/null; }
wait_for() { # wait_for <seconds> <cmd...>
    secs=$1
    shift
    deadline=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        "$@" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

pkill -x testclient 2>/dev/null
sleep 0.5

"$fx" --name mm1 --at 100,100 >/dev/null 2>&1 &
sleep 0.4
"$fx" --name mm2 --at 300,300 >/dev/null 2>&1 &
sleep 0.6
c1=$(xprop -root _NET_CLIENT_LIST | grep -o '0x[0-9a-f]*' | head -1)
[ -n "$c1" ] || { echo "multimon: no clients appeared" >&2; exit 2; }

printf 'multimon: split 1280 -> 2x640\n'
"$poke" 640x800+0+0 640x800+640+0
wait_for 5 w_lt "$c1" 640 &&
    ok "clients re-tile into narrowed head monitor" ||
    nope "clients re-tile into narrowed head monitor"

printf 'multimon: migrate workspaces between monitors\n'
"$here/scripts/inject-key.sh" super+ctrl+m
wait_for 5 desktop_is 8 &&
    ok "head monitor now shows workspace 9" ||
    nope "head monitor now shows workspace 9"

printf 'multimon: spawn follows migrated focus\n'
"$fx" --name mm3 --at 200,200 >/dev/null 2>&1 &
sleep 0.6
c3=$(xprop -root _NET_CLIENT_LIST | grep -o '0x[0-9a-f]*' | tail -1)
ws3=$(xprop -id "$c3" _NET_WM_DESKTOP 2>/dev/null | grep -o '[0-9]*$')
[ "$ws3" = "8" ] &&
    ok "new client lands on focused monitor workspace" ||
    nope "new client lands on focused monitor workspace ($ws3)"

printf 'multimon: merge back to single monitor\n'
"$poke" 1280x800+0+0
wait_for 5 w_ge "$c1" 640 &&
    ok "merge restores full-width tiling" ||
    nope "merge restores full-width tiling"

alive && ok "WM alive after topology churn" || nope "WM alive after topology churn"

pkill -x testclient 2>/dev/null
printf 'multimon: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]

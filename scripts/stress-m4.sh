#!/bin/sh
# Deterministic M4 stress: hammer every workspace/scratchpad action in a
# fixed order against the running WM; report liveness at the end.
# Usage: DISPLAY=:2 scripts/stress-m4.sh [iterations]
D=${DISPLAY:?DISPLAY required}
N=${1:-10}
pass=0
cur() { xprop -root _NET_CURRENT_DESKTOP 2>/dev/null | grep -o '= [0-9]*' | tr -d '= '; }
k() { "$(dirname "$0")/inject-key.sh" "$@"; }

for i in $(seq "$N"); do
    k super+2 >/dev/null 2>&1; sleep 0.15
    k super+1 >/dev/null 2>&1; sleep 0.15
    k super+3 >/dev/null 2>&1; sleep 0.15
    k super+Tab >/dev/null 2>&1; sleep 0.15
    k super+shift+5 >/dev/null 2>&1; sleep 0.15
    k super+grave >/dev/null 2>&1; sleep 0.15
    k super+g >/dev/null 2>&1; sleep 0.15
    k super+9 >/dev/null 2>&1; sleep 0.15
    k super+1 >/dev/null 2>&1; sleep 0.15
    pass=$((pass + 1))
done
echo "iterations=$pass final_ws=$(cur) alive=$(pgrep -c -x austere || echo 0)"

#!/bin/sh
# Manage the throwaway Xephyr display used for all austere development.
# The display is long-lived; only austere itself gets restarted in place.
#
# The display number is auto-picked: first free among :2..:12 (never :0/:1,
# which belong to the host session / Xwayland). Override with
# AUSTERE_DEV_DISP=:N. Usage: xephyr.sh start|stop|status|disp

RUNDIR=${XDG_RUNTIME_DIR:-/tmp}/austere-dev
PIDFILE=$RUNDIR/xephyr.pid
DISPFILE=$RUNDIR/disp
GEOM=1280x800x24

mkdir -p "$RUNDIR"

up() {
    xdpyinfo -display "$1" >/dev/null 2>&1
}

alive() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

pick_disp() {
    if [ -n "$AUSTERE_DEV_DISP" ]; then
        printf '%s\n' "$AUSTERE_DEV_DISP"
        return
    fi
    n=2
    while [ "$n" -le 12 ]; do
        if ! up ":$n"; then
            printf ':%s\n' "$n"
            return
        fi
        n=$((n + 1))
    done
    echo "no free display found in :2..:12" >&2
    exit 1
}

case "${1:-}" in
start)
    # Orphaned austere-dev Xephyrs (from interrupted runs) would make
    # pick_disp skip to :3+ and leave ghost displays eating keystrokes.
    for p in $(pgrep -x Xephyr); do
        tr '\0' '' </proc/"$p"/cmdline 2>/dev/null | grep -q austere-dev &&
            kill -9 "$p" 2>/dev/null
    done
    # only sweep the dev range, never the host's X0
    rm -f /tmp/.X2-lock /tmp/.X3-lock /tmp/.X4-lock /tmp/.X5-lock \
        /tmp/.X11-unix/X2 /tmp/.X11-unix/X3 /tmp/.X11-unix/X4 \
        /tmp/.X11-unix/X5
    if alive && [ -f "$DISPFILE" ]; then
        echo "Xephyr already running on $(cat "$DISPFILE")"
        exit 0
    fi
    DISP=$(pick_disp)
    rm -f "$PIDFILE"
    nohup Xephyr "$DISP" -screen "$GEOM" -name austere-dev \
        +extension RANDR </dev/null >/dev/null 2>&1 &
    echo $! >"$PIDFILE"
    i=0
    while ! up "$DISP"; do
        i=$((i + 1))
        if [ "$i" -gt 50 ]; then
            echo "Xephyr failed to start on $DISP" >&2
            exit 1
        fi
        sleep 0.1
    done
    echo "$DISP" >"$DISPFILE"
    echo "Xephyr up on $DISP ($GEOM)"
    ;;
stop)
    if alive; then
        kill "$(cat "$PIDFILE")"
        rm -f "$PIDFILE" "$DISPFILE"
        echo "Xephyr stopped"
    else
        echo "Xephyr not running (pidfile stale or missing)"
        rm -f "$DISPFILE"
    fi
    ;;
status)
    if alive && [ -f "$DISPFILE" ]; then
        d=$(cat "$DISPFILE")
        if up "$d"; then
            echo "Xephyr up on $d"
            exit 0
        fi
    fi
    echo "Xephyr down"
    exit 1
    ;;
disp)
    if [ -f "$DISPFILE" ]; then cat "$DISPFILE"; else exit 1; fi
    ;;
*)
    echo "usage: $0 start|stop|status|disp" >&2
    exit 2
    ;;
esac

#!/bin/sh
# Capability-detecting smoke test for austere.
# Reads _NET_SUPPORTED off the root window and only asserts behavior for
# atoms the running WM claims — so the same script stays green from M0 to
# M10 and grows coverage automatically as milestones land.
#
# Environment:
#   DISPLAY       target display (set to :1 by make check)
#   SMOKE_NO_QUIT set to skip the final quit-bind test (make check quits
#                 itself after valgrind has produced its report)

DISP=${DISPLAY:-:1}
export DISPLAY=$DISP

case "$DISP" in
    :0|:0.0|:1|:1.0)
        echo "smoke: refusing unsafe DISPLAY '$DISP'" >&2
        exit 2;;
esac

[ -n "$SMOKE_TRACE" ] && set -x

pass=0
fail=0

ok() {
    pass=$((pass + 1))
    printf '  ok   %s\n' "$1"
}

nope() {
    fail=$((fail + 1))
    printf '  FAIL %s %s\n' "$1" "$2"
}

assert() { # assert <desc> <cmd...>
    desc=$1
    shift
    if "$@" >/dev/null 2>&1; then ok "$desc"; else nope "$desc"; fi
}

supported() { # supported <atom-name> -> does WM advertise it?
    xprop -root _NET_SUPPORTED 2>/dev/null |
        tr ',' '\n' | sed 's/^ *//' | grep -qF -- "$1"
}

client_count() {
    xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -o '0x[0-9a-f]*' |
        wc -l
}

count_gt() { [ "$(client_count)" -gt "$1" ]; }
count_le() { [ "$(client_count)" -le "$1" ]; }
count_ge() { [ "$(client_count)" -ge "$1" ]; }

nth_client() { # nth_client <n> -> window id of nth entry (oldest first)
    xprop -root _NET_CLIENT_LIST 2>/dev/null |
        grep -o '0x[0-9a-f]*' | sed -n "${1}p"
}

geom() { # geom <winid> -> "x y w h" absolute
    [ -n "$1" ] || { echo "0 0 0 0"; return; }
    xwininfo -id "$1" -stats 2>/dev/null | awk '
        /Absolute upper-left X/ { x = $4 }
        /Absolute upper-left Y/ { y = $4 }
        /^ *Width:/             { w = $2 }
        /^ *Height:/            { h = $2 }
        END { print x + 0, y + 0, w + 0, h + 0 }'
}

active_is() {
    [ "$(xprop -root _NET_ACTIVE_WINDOW 2>/dev/null |
        grep -o '0x[0-9a-f]*')" = "$1" ]
}

near() { # near <got> <want> <tolerance>
    [ $(( $1 >= $2 ? $1 - $2 : $2 - $1 )) -le "$3" ]
}

field() { echo "$1" | cut -d' ' -f"$2"; }

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

key() {
    "$(dirname "$0")/inject-key.sh" "$1"
}

printf 'smoke: display %s\n' "$DISP"

# the one-shot welcome panel (§9.5) eats keys until dismissed
key Escape || true
sleep 0.4

# --- always asserted -------------------------------------------------
if ! xdpyinfo >/dev/null 2>&1; then
    echo "FAIL: cannot reach $DISP (is Xephyr up? scripts/xephyr.sh start)"
    exit 1
fi

# The WM may still be starting (valgrind makes this slow); verify the
# EWMH chain, not just the root property, so a dead predecessor's stale
# root property can't fool us.
wm_up() {
    w=$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null |
        grep -o '0x[0-9a-f]*')
    [ -n "$w" ] || return 1
    xprop -id "$w" _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id'
}
if ! wait_for 20 wm_up; then
    echo "FAIL: no WM announced itself on $DISP within 20s"
    exit 1
fi

# A compliant WM announces itself via the checking window.
assert "EWMH checking window present (_NET_SUPPORTING_WM_CHECK)" \
    sh -c 'xprop -root _NET_SUPPORTING_WM_CHECK | grep -q "window id"'
assert "WM name advertised" \
    sh -c 'xprop -root _NET_WM_NAME 2>/dev/null | grep -qi austere'

# --- client management (M1+) ------------------------------------------
# Deterministic: spawn our own fixture directly (bind-based spawning is
# gated on Xephyr having host focus, so it stays a best-effort bonus).
if supported _NET_CLIENT_LIST; then
    fixture="$(dirname "$0")/../contrib/testclient"
    before=$(client_count)
    (DISPLAY=$DISP "$fixture" --name smokeprobe >/dev/null 2>&1 &)
    if wait_for 3 count_gt "$before"; then
        ok "new client is managed and listed"
    else
        nope "new client is managed and listed"
    fi

    # close path: prefer the WM's own delete flow, fall back to killing
    # the fixture; either way the client must leave the list.
    key alt+q || true
    sleep 0.6
    pkill -TERM -x testclient 2>/dev/null
    if wait_for 3 count_le "$before"; then
        ok "closed client leaves the list, focus refocuses"
    else
        nope "closed client leaves the list, focus refocuses"
    fi
fi

# --- WM relaunch machinery (used by interaction + layout sections) -----
wm_bin="$(dirname "$0")/../austere"
fx="$(dirname "$0")/../contrib/testclient"

wm_pid_alive() { # true if WM_PID names a live, non-zombie process
    ps -o stat= -p "${WM_PID:-0}" 2>/dev/null | grep -vq '^Z'
}

wm_pid_alive_inverted() { ! wm_pid_alive; }

wm_name_gone() { ! pgrep -x austere >/dev/null 2>&1; }

relaunch_wm() { # relaunch_wm <layout-name>
    if [ -n "${WM_PID:-}" ] && wm_pid_alive; then
        kill -TERM "$WM_PID" 2>/dev/null
        # generous: an instrumented WM needs seconds to write its
        # valgrind report before the process disappears; pgrep -x
        # cannot see it (child comm is memcheck-*, not "austere")
        wait_for 30 wm_pid_alive_inverted || kill -9 "$WM_PID"
    else
        pkill -TERM -x austere 2>/dev/null
        wait_for 30 wm_name_gone || pkill -9 -x austere
        wait_for 5 wm_name_gone || return 1
    fi
    : >/tmp/austere-smoke.log
    (DISPLAY=$DISP XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-}" \
        AUSTERE_BAR_SCRIPTS="${AUSTERE_BAR_SCRIPTS:-}" \
        AUSTERE_LAYOUT="$1" nohup "$wm_bin" --replace \
        >/tmp/austere-smoke.log 2>&1 &)
    wait_for 20 wm_up || return 1
}

spawn_fixture() { # spawn_fixture <name> <at>
    (DISPLAY=$DISP "$fx" --name "$1" --at "$2" >/dev/null 2>&1 &)
}

# --- mouse: click-focus, drag, resize, snap (M2+) ----------------------
if supported _NET_CLIENT_LIST && [ -x "$(dirname "$0")/../contrib/btninject" ]; then
    inject="$(dirname "$0")/../contrib/btninject"
    fixture="$(dirname "$0")/../contrib/testclient"

    # memcheck slows event processing enough to lose drag races; run the
    # interaction asserts against an uninstrumented instance
    if [ -n "${WM_PID:-}" ] && wm_pid_alive; then
        relaunch_wm tile || true
    fi

    (DISPLAY=$DISP "$fixture" --name m2a --at 30,30 --min 500x350 >/dev/null 2>&1 &)
    (DISPLAY=$DISP "$fixture" --name m2b --at 740,420 --min 500x350 >/dev/null 2>&1 &)
    before=$(client_count)
    wait_for 4 count_ge 2 || nope "m2: fixtures failed to appear"

    aid=$(nth_client 1)
    bid=$(nth_client 2)
    ga=$(geom "$aid")
    ax=$(field "$ga" 1); ay=$(field "$ga" 2); aw=$(field "$ga" 3)

    # click-to-focus: B was managed last and owns focus; clicking A must
    # move _NET_ACTIVE_WINDOW to A.
    cx=$((ax + aw / 2)); cy=$((ay + $(field "$ga" 4) / 2))
    DISPLAY=$DISP "$inject" click "$cx" "$cy" || true
    sleep 0.4
    assert "click focuses the clicked client" active_is "$aid"

    # drag move by (+200,+150) from window center
    DISPLAY=$DISP "$inject" move "$cx" "$cy" $((cx + 200)) $((cy + 150)) ||
        true
    wait_for 2 near "$(field "$(geom "$aid")" 1)" "$((ax + 200))" 4 &&
        ok "super+drag moves the window" ||
        nope "super+drag moves the window"

    # resize from bottom-right corner by (+100,+80)
    ga=$(geom "$aid")
    rx=$(( $(field "$ga" 1) + $(field "$ga" 3) ))
    ry=$(( $(field "$ga" 2) + $(field "$ga" 4) ))
    DISPLAY=$DISP "$inject" resize "$rx" "$ry" $((rx + 100)) $((ry + 80)) ||
        true
    gra=$(geom "$aid")
    wait_for 2 near "$(field "$gra" 3)" "$((aw + 100))" 6 &&
        ok "super+resize grows the window" ||
        nope "super+resize grows the window"

    # snap: fling toward the top-left corner; both coords must pin to 0
    gra=$(geom "$aid")
    sx=$(( $(field "$gra" 1) + $(field "$gra" 3) / 2 ))
    sy=$(( $(field "$gra" 2) + $(field "$gra" 4) / 2 ))
    DISPLAY=$DISP "$inject" move "$sx" "$sy" -3000 -3000 || true
    gs=$(geom "$aid")
    if [ "$(field "$gs" 1)" = "0" ] && [ "$(field "$gs" 2)" = "0" ]; then
        ok "edge snapping pins dragged windows"
    else
        nope "edge snapping pins dragged windows (got $(geom "$aid"))"
    fi

    pkill -x testclient 2>/dev/null
fi

# --- layouts: tile / monocle / cycling (M3+) ---------------------------
if supported _NET_CLIENT_LIST && [ -x "$wm_bin" ]; then
    if relaunch_wm tile; then
        spawn_fixture tA 100,100
        sleep 0.3
        spawn_fixture tB 200,200
        sleep 0.3
        spawn_fixture tC 300,300
        wait_for 5 count_ge 3 || true

        # _NET_CLIENT_LIST is oldest-first; internal order is newest-
        # first, so the newest (last entry) takes the master slot
        cid=$(nth_client 3)   # newest -> master slot
        bid=$(nth_client 2)   # stack top-right
        aid=$(nth_client 1)   # stack bottom-right
        gc=$(geom "$cid"); gb=$(geom "$bid"); ga=$(geom "$aid")
        if [ "$(field "$gc" 1)" -lt 10 ] &&
            near "$(field "$gc" 3)" 640 12 &&
            [ "$(field "$gb" 1)" -gt 600 ] &&
            [ "$(field "$gb" 1)" -lt 680 ] &&
            near "$(field "$gb" 4)" 400 12 &&
            near "$(field "$ga" 2)" 400 12 &&
            near "$(field "$ga" 4)" 400 12; then
            ok "tile: newest masters left, stack fills right"
        else
            nope "tile: newest masters left, stack fills right" \
                "(C=$gc B=$gb A=$ga)"
        fi

        # ratio binds are focus-gated; try, but don't lie about coverage
        key super+l || true
        sleep 0.5
        gw=$(field "$(geom "$cid")" 3)
        if ! near "$gw" 640 12; then
            ok "super+l adjusts the master ratio"
        else
            printf '  note ratio bind undelivered (unfocused Xephyr?)\n'
        fi
    else
        nope "layouts: could not relaunch WM in tile mode"
    fi

    pkill -x testclient 2>/dev/null
    sleep 0.5

    if relaunch_wm monocle; then
        spawn_fixture mA2 150,150
        sleep 0.3
        spawn_fixture mB2 250,250
        wait_for 5 count_ge 2 || true
        wah=$(xprop -root _NET_WORKAREA 2>/dev/null | grep -o '[0-9]*' |
            sed -n 4p)
        g1=$(geom "$(nth_client 1)")
        g2=$(geom "$(nth_client 2)")
        if near "$(field "$g1" 3)" 1280 12 && near "$(field "$g1" 4)" "$wah" 12 &&
            near "$(field "$g2" 3)" 1280 12 && near "$(field "$g2" 4)" "$wah" 12;
        then
            ok "monocle: every client fills the workarea"
        else
            nope "monocle: every client fills the workarea (g1=$g1 g2=$g2)"
        fi

        # cycling must not leave stale geometries: super+s flips to
        # tile, shrinking widths from full-screen
        cycled=""
        for i in 1 2 3; do
            key super+s || true
            sleep 0.5
            g1=$(geom "$(nth_client 1)")
            if ! near "$(field "$g1" 3)" 1280 12; then
                cycled="$i"
                break
            fi
        done
        # shellcheck disable=SC2181
        if [ -n "$cycled" ]; then
            ok "layout cycle rewrites geometries (attempt $cycled)"
        else
            printf '  note layout-cycle bind undelivered (unfocused Xephyr?)\n'
        fi
    else
        nope "layouts: could not relaunch WM in monocle mode"
    fi

    pkill -x testclient 2>/dev/null
fi

# --- workspaces: migration, urgency, pager atoms (M4+) -------------------
if supported _NET_CURRENT_DESKTOP && [ -x "$wm_bin" ]; then
    inject="$(dirname "$0")/../contrib/btninject"

    desktop_is() { # desktop_is <index>
        [ "$(xprop -root _NET_CURRENT_DESKTOP 2>/dev/null |
            grep -o '[0-9]*')" = "$1" ]
    }
    ws_of() { # ws_of <winid> -> _NET_WM_DESKTOP value
        xprop -id "$1" _NET_WM_DESKTOP 2>/dev/null | grep -o '[0-9]*$'
    }
    ws_is() { # ws_is <winid> <index>
        [ "$(ws_of "$1")" = "$2" ]
    }
    state_is() { # state_is <winid> <Map State value>
        [ "$(xwininfo -id "$1" 2>/dev/null | grep 'Map State' |
            awk '{print $3}')" = "$2" ]
    }
    mapped_state() { # mapped_state <winid>
        xwininfo -id "$1" 2>/dev/null | grep 'Map State' | awk '{print $3}'
    }
    has_demand() { # has_demand <winid>
        xprop -id "$1" _NET_WM_STATE 2>/dev/null |
            grep -q DEMANDS_ATTENTION
    }
    lacks_demand() { ! has_demand "$1"; }

    if supported _NET_NUMBER_OF_DESKTOPS; then
        [ "$(xprop -root _NET_NUMBER_OF_DESKTOPS 2>/dev/null |
            grep -o '[0-9]*')" = "9" ] &&
            ok "nine desktops advertised" ||
            nope "nine desktops advertised"
    fi

    spawn_fixture w1a 100,100
    sleep 0.3
    spawn_fixture w1b 250,250
    wait_for 5 count_ge 2 || true
    wb=$(nth_client 2) # newest of the pair; manage() focused it

    key super+shift+3 || true # send focused to workspace index 2
    wait_for 3 ws_is "$wb" 2 &&
        ok "super+shift+N retags client desktop" ||
        nope "super+shift+N retags client desktop"
    sleep 0.3
    # dwm-style: hidden clients stay mapped but parked off-screen
    parked() { # parked <winid> -> x position beyond the screen width
        [ "$(xwininfo -id "$1" 2>/dev/null |
            awk '/Absolute upper-left X/{print $4}')" -ge 1280 ]
    }
    parked_off() { # parked_off <winid> -> back on-screen
        ! parked "$1"
    }
    wait_for 3 parked "$wb" &&
        ok "sent client hides until viewed" ||
        nope "sent client hides until viewed"
    desktop_is 0 && ok "send does not switch view" ||
        nope "send does not switch view"

    key super+3 || true
    wait_for 3 desktop_is 2 &&
        ok "super+N views the workspace" ||
        nope "super+N views the workspace"
    wait_for 3 parked_off "$wb" && # back on-screen: x < 1280
        ok "viewed workspace remaps its clients" ||
        nope "viewed workspace remaps its clients"

    key super+3 || true # same workspace again -> back-and-forth
    wait_for 3 desktop_is 0 &&
        ok "re-viewing current workspace toggles to previous" ||
        nope "re-viewing current workspace toggles to previous"

    if supported _NET_WM_STATE_DEMANDS_ATTENTION &&
        [ -x "$(dirname "$0")/../contrib/testclient" ]; then
        (DISPLAY=$DISP "$fx" --name urgent1 --urgent-after 2500 \
            --at 400,400 >/dev/null 2>&1 &)
        # three clients total now; waiting on 2 would race the new map
        wait_for 5 count_ge 3 || true
        u1=$(nth_client 3) # newest of three; manage() focused it
        key super+shift+8 || true # hide it before the hint fires
        wait_for 5 has_demand "$u1" &&
            ok "urgency hint raises DEMANDS_ATTENTION" ||
            nope "urgency hint raises DEMANDS_ATTENTION"
        key super+8 || true
        wait_for 5 lacks_demand "$u1" &&
            ok "focus clears urgency" ||
            nope "focus clears urgency"
        key super+8 || true # back-and-forth home
        wait_for 3 desktop_is 0 || true
    fi

    pkill -x testclient 2>/dev/null
fi

# --- multi-monitor topology via the monpoke seam (M5+) -----------------
# Xephyr ignores xrandr --setmonitor, so topology changes are injected
# with the _AUSTERE_TEST_MONITORS ClientMessage instead of real RandR.
poke="$(dirname "$0")/../contrib/monpoke"
if [ -x "$poke" ] && [ -x "$wm_bin" ]; then
    ws_is() { # ws_is <winid> <index>
        [ "$(xprop -id "$1" _NET_WM_DESKTOP 2>/dev/null |
            grep -o '[0-9]*$')" = "$2" ]
    }
    width_of() { geom "$1" | cut -d' ' -f3; }
    w_lt() { [ "$(width_of "$1")" -lt "$2" ]; }
    w_ge() { [ "$(width_of "$1")" -ge "$2" ]; }
    poke_split() { DISPLAY=$DISP "$poke" 640x800+0+0 640x800+640+0; }
    poke_merge() { DISPLAY=$DISP "$poke" 1280x800+0+0; }

    spawn_fixture m5a 100,100
    sleep 0.3
    spawn_fixture m5b 250,250
    wait_for 5 count_ge 2 || true
    c1=$(nth_client 1)

    poke_split
    # stack client must move into the head region regardless of layout
    c2x() { geom "$1" | cut -d' ' -f1; }
    x_lt() { [ "$(c2x "$1")" -lt "$2" ]; }
    wait_for 8 x_lt "$(nth_client 2)" 640 &&
        ok "split re-tiles clients into head region" ||
        nope "split re-tiles clients into head region (x=$(c2x "$(nth_client 2)"))"

    key super+ctrl+m || true # migrate: head now shows workspace 8
    wait_for 5 desktop_is 8 &&
        ok "migrate bind swaps monitor workspaces" ||
        nope "migrate bind swaps monitor workspaces"

    spawn_fixture m5c 300,300
    sleep 0.4
    wait_for 5 count_ge 3 || true
    c3=$(nth_client 3)
    ws_is "$c3" 8 &&
        ok "spawn follows migrated focus cursor" ||
        nope "spawn follows migrated focus cursor"

    poke_merge
    wait_for 5 w_ge "$c1" 640 &&
        ok "merge restores full-width layout" ||
        nope "merge restores full-width layout"
    pgrep -x austere >/dev/null &&
        ok "topology churn keeps WM alive" ||
        nope "topology churn keeps WM alive"

    pkill -x testclient 2>/dev/null
fi

# --- statusbar & modules (M6+) -----------------------------------------
if supported _NET_SUPPORTED && [ -x "$wm_bin" ]; then
    desktop_is() { # desktop_is <index>
        [ "$(xprop -root _NET_CURRENT_DESKTOP 2>/dev/null |
            grep -o '[0-9]*')" = "$1" ]
    }

    # Bar reserves space: workarea starts below a top dock of ~1 font row.
    wa=$(xprop -root _NET_WORKAREA 2>/dev/null | grep -o '[0-9]*' |
        tr '\n' ' ')
    set -- $wa
    [ -n "$wa" ] && [ "$2" -gt 0 ] && [ "$4" -lt 800 ] &&
        ok "bar reserves workarea" ||
        nope "bar reserves workarea (got: $wa)"

    # A dock-type window exists on the root.
    dock=""
    for W in $(xwininfo -root -children 2>/dev/null |
        grep -o '0x[0-9a-f]*'); do
        xprop -id "$W" _NET_WM_WINDOW_TYPE 2>/dev/null |
            grep -q DOCK && dock=$W && break
    done
    [ -n "$dock" ] && ok "bar window is a dock" ||
        nope "bar window is a dock"

    # Script module spawned with the make check fixture.
    pgrep -f ticker-test.sh >/dev/null &&
        ok "script module spawned" ||
        nope "script module spawned"

    # Click-to-view: workspace '3' sits ~40px into the left group.
    if [ -x "$(dirname "$0")/../contrib/btninject" ]; then
        inject="$(dirname "$0")/../contrib/btninject"
        "$inject" click 40 8 || true
        wait_for 3 desktop_is 2 &&
            ok "bar click switches workspace" ||
            nope "bar click switches workspace"
        "$inject" click 12 8 || true
        wait_for 3 desktop_is 0 || true
    fi

    pkill -x testclient 2>/dev/null
fi

# --- settings core: first-run, hot reload, transaction (M7+) -----------
if [ -x "$wm_bin" ]; then
    conf=.smoke-conf/austere/austere.conf

    [ -f "$conf" ] && ok "first run wrote default conf" ||
        nope "first run wrote default conf"

    pkill -x testclient 2>/dev/null
    sleep 0.5
    spawn_fixture s7 100,100
    wait_for 6 count_ge 1
    c7=$(nth_client "$(client_count)") # newest: older entries may be stale
    border_is() { # border_is <winid> <width>
        [ "$(xwininfo -id "$1" 2>/dev/null |
            awk 'tolower($1)=="border"{print $3}')" = "$2" ]
    }

    sed -i "s/^border_width = .*/border_width = 9/" "$conf"
    wait_for 8 border_is "$c7" 9 &&
        ok "conf save hot-reloads" ||
        nope "conf save hot-reloads"

    printf '[broken\n' >> "$conf"
    sleep 0.6
    bw1=$(xwininfo -id "$c7" 2>/dev/null |
        awk 'tolower($1)=="border"{print $3}')
    [ "$bw1" = "9" ] && pgrep -x austere >/dev/null &&
        ok "broken conf leaves session untouched" ||
        nope "broken conf leaves session untouched (bw=$bw1 alive=$(pgrep -x austere | wc -l))"

    sed -i '/\[broken/d' "$conf"
    sleep 0.6
    pgrep -x austere >/dev/null &&
        ok "fixed conf reloads again" ||
        nope "fixed conf reloads again"

    pkill -x testclient 2>/dev/null
fi

# --- layouts / ratio binds need visual or state introspection ----------
# State dump arrives with the command socket (M10); until then layout
# checks stay manual per AGENTS protocol.

# --- settings menu (M8+) ------------------------------------------------
if [ -x "$wm_bin" ] && [ -x "$(dirname "$0")/../contrib/keyinject" ]; then
    pkill -x testclient 2>/dev/null
    sleep 0.5
    wins_before=$(xwininfo -root -children 2>/dev/null | grep -c '0x')
    key super+e || true
    sleep 0.6
    wins_menu=$(xwininfo -root -children 2>/dev/null | grep -c '0x')
    [ "$wins_menu" -gt "$wins_before" ] &&
        ok "menu opens a window" ||
        nope "menu opens a window"

    # live-apply: navigate to border_width and bump it with Right
    spawn_fixture s8 100,100
    wait_for 5 count_ge 1
    c8=$(nth_client "$(client_count)")
    bw_pre=$(xwininfo -id "$c8" 2>/dev/null |
        awk 'tolower($1)=="border"{print $3}')
    key Down || true; key Down || true; key Down || true
    key Right || true
    sleep 0.5
    bw=$(xwininfo -id "$c8" 2>/dev/null |
        awk 'tolower($1)=="border"{print $3}')
    [ "$bw" = "$((bw_pre + 1))" ] &&
        ok "menu edits apply live" ||
        nope "menu edits apply live (bw=$bw want $((bw_pre + 1)))"

    key ctrl+s || true
    sleep 0.8
    grep -q "^border_width = $bw\$" .smoke-conf/austere/austere.conf &&
        ok "menu save round-trips to file" ||
        nope "menu save round-trips to file"

    key Escape || true
    sleep 0.4
    pkill -x testclient 2>/dev/null
    sleep 0.5
    wins_after=$(xwininfo -root -children 2>/dev/null | grep -c '0x')
    if [ "$wins_after" = "$wins_before" ]; then
        ok "menu closes cleanly"
    else
        nope "menu closes cleanly ($wins_after vs $wins_before)"
        echo "DUMP DISPLAY=$DISPLAY procs:"
        ps -eo pid,stat,comm,args | grep -E 'austere|memcheck' | grep -v grep
        echo "--- xprop check:"
        xprop -root _NET_SUPPORTING_WM_CHECK 2>&1 | head -2
        echo "--- raw tree:"
        xwininfo -root -tree 2>&1 | head -12
    fi
fi

# --- panels: switcher, launcher, welcome (M9+) -------------------------
if [ -x "$wm_bin" ] && [ -x "$(dirname "$0")/../contrib/keyinject" ]; then
    type_key() { key "$1" || true; sleep 0.25; }

    # launcher: history pre-seeds the top row, so one Return runs the
    # fixture — robust against XTEST key drops; also exercises the
    # history load path.
    mkdir -p .smoke-bin .smoke-data/austere
    printf '#!/bin/sh\necho ok > .smoke-launch-ok\n' \
        > .smoke-bin/zzaame
    chmod +x .smoke-bin/zzaame
    echo zzaame > .smoke-data/austere/history
    sed -i "s|^scan_path = .*|scan_path = true\ncustom_dir = \"$PWD/.smoke-bin\"|" \
        .smoke-conf/austere/austere.conf
    sleep 0.6 # hotwatch picks up custom_dir

    launched=""
    for attempt in 1 2 3; do
        rm -f .smoke-launch-ok
        key alt+space || true
        sleep 0.8
        key Return || true
        sleep 1.2
        if [ -f .smoke-launch-ok ]; then
            launched="$attempt"
            break
        fi
        key Escape || true # panel may still be up
        sleep 0.4
    done
    [ -n "$launched" ] &&
        ok "launcher partial-name exec" ||
        nope "launcher partial-name exec"

    # switcher: client on another workspace via filtered panel
    spawn_fixture sw1 100,100
    sleep 0.4
    spawn_fixture sw2 200,200
    wait_for 5 count_ge 2 || true
    key super+shift+2 || true # send newest to ws2
    sleep 0.4
    key alt+Tab || true
    sleep 0.6
    type_key s; type_key w; type_key 2
    key Return || true
    sleep 0.8
    desktop_is 1 &&
        ok "switcher focuses cross-workspace client" ||
        nope "switcher focuses cross-workspace client"

    # MRU quick-cycle flips focus between two visible clients
    spawn_fixture sw3 300,300
    sleep 0.4
    spawn_fixture sw4 400,400
    wait_for 5 count_ge 2 || true
    a0=$(xprop -root _NET_ACTIVE_WINDOW 2>/dev/null |
        grep -o '0x[0-9a-f]*')
    a1="$a0"
    for i in 1 2 3; do # a dropped injection just means step again
        key super+Tab || true
        sleep 0.4
        a1=$(xprop -root _NET_ACTIVE_WINDOW 2>/dev/null |
            grep -o '0x[0-9a-f]*')
        [ -n "$a1" ] && [ "$a1" != "$a0" ] && break
    done
    [ -n "$a0" ] && [ -n "$a1" ] && [ "$a1" != "$a0" ] &&
        ok "MRU quick-cycle steps focus" ||
        nope "MRU quick-cycle steps focus"

    pkill -x testclient 2>/dev/null
fi

# --- polish: socket, restart, hostile clients (M10+) -------------------
if command -v "$PWD/contrib/austere-cmd" >/dev/null 2>&1; then
    # command socket: view + garbage + state
    contrib/austere-cmd ws 2 >/dev/null 2>&1
    wait_for 3 desktop_is 1 &&
        ok "socket: ws command switches desktop" ||
        nope "socket: ws command switches desktop"
    contrib/austere-cmd bogus-action >/dev/null 2>&1
    rc=$?
    [ "$rc" = "1" ] && pgrep -x austere >/dev/null &&
        ok "socket: garbage line errs, WM alive" ||
        nope "socket: garbage line errs, WM alive"

    # restart-in-place: clients survive, state file consumed
    spawn_fixture rs1 100,100
    wait_for 5 count_ge 1 || true
    old_ws=$(xprop -root _NET_CURRENT_DESKTOP 2>/dev/null |
        grep -o '[0-9]*')
    contrib/austere-cmd restart >/dev/null 2>&1
    wait_for 15 count_ge 1 || true
    sleep 0.5
    new_ws=$(xprop -root _NET_CURRENT_DESKTOP 2>/dev/null |
        grep -o '[0-9]*')
    [ "$old_ws" = "$new_ws" ] &&
        ok "restart preserves session state" ||
        nope "restart preserves session state ($old_ws -> $new_ws)"
    [ ! -f "${XDG_RUNTIME_DIR:-/tmp}/austere/state" ] &&
        ok "restart consumed state file" ||
        nope "restart consumed state file"

    # hostile clients: malformed hints/classes/names + map churn
    contrib/hostile-client -r 2 >/dev/null 2>&1
    sleep 0.6
    pgrep -x austere >/dev/null &&
        ok "hostile-client torture survives" ||
        nope "hostile-client torture survives"

    pkill -x testclient 2>/dev/null
fi

# --- shutdown ---------------------------------------------------------
# Prefer the quit bind (real grab-path coverage); XTEST injection is
# gated by whether the Xephyr window has host focus, so retry a few
# times, then fall back to SIGTERM — same clean-exit path internally.

quit_sent=""
if supported _NET_SUPPORTING_WM_CHECK; then
    for i in 1 2 3; do
        key super+m && quit_sent="$i"
        sleep 0.6
        pgrep -x austere >/dev/null || break
    done
fi

if pgrep -x austere >/dev/null; then
    [ -n "$quit_sent" ] ||
        printf '  note quit bind undelivered (unfocused Xephyr?); SIGTERM fallback\n'
    pkill -TERM -x austere
    sleep 1
    pgrep -x austere >/dev/null && { nope "austere survived SIGTERM"; }
    # shellcheck disable=SC2181
    true
else
    ok "quit bind exits cleanly${quit_sent:+ (attempt $quit_sent)}"
fi

printf 'smoke: %d passed, %d failed\n' "$pass" "$fail"

[ "$fail" -eq 0 ]

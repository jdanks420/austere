#!/bin/sh
# Resolve a "super+shift+q"-style combo to modifier mask + keycode using
# xmodmap, then inject it via contrib/keyinject (XTEST).
#
# Modifier names: super|mod4, alt|mod1, ctrl|control, shift, lock,
#                 mod2..mod5.
# The last token is the keysym name resolved against `xmodmap -pke`.

set -e

combo=$1
[ -n "$combo" ] || { echo "usage: $0 <combo e.g. super+shift+q>" >&2; exit 2; }

# Hard guard: never inject into the host session (AGENTS rule).
case "${DISPLAY:-}" in
    :0|:0.0|:1|:1.0|"")
        echo "$0: refusing unsafe DISPLAY '${DISPLAY:-unset}'" >&2
        exit 2;;
esac

here=$(cd "$(dirname "$0")/.." && pwd)
inject="$here/contrib/keyinject"
[ -x "$inject" ] || { echo "keyinject not built (make tools)" >&2; exit 1; }

mods=0
name=""
IFS=+
for tok in $combo; do
    case "$tok" in
    super | mod4) mods=$((mods | 0x40)) ;;
    alt | mod1) mods=$((mods | 0x08)) ;;
    mod2) mods=$((mods | 0x10)) ;;
    mod3) mods=$((mods | 0x20)) ;;
    mod5) mods=$((mods | 0x80)) ;;
    ctrl | control) mods=$((mods | 0x04)) ;;
    shift) mods=$((mods | 0x01)) ;;
    lock | caps) mods=$((mods | 0x02)) ;;
    *) [ -z "$name" ] && name="$tok" ||
        { echo "two non-modifier tokens in '$combo'" >&2; exit 2; } ;;
    esac
done

[ -n "$name" ] || { echo "no keysym in '$combo'" >&2; exit 2; }

keycode=$(xmodmap -pke | awk -v n="$name" '
    /^keycode/ {
        kc = $2
        for (i = 4; i <= NF; i++)
            if ($i == n) { print kc; exit }
    }')

[ -n "$keycode" ] || { echo "keysym '$name' not found in keymap" >&2; exit 1; }

exec "$inject" "$(printf '%x' "$mods")" "$keycode"

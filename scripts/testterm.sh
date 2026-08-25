#!/bin/sh
# Stand-in "terminal" for smoke tests: austere's spawn action runs this
# via AUSTERE_TERMINAL; it just opens a managed test window.
exec "$(dirname "$0")/../contrib/testclient" --name testterm "$@"

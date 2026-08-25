#!/bin/sh
# Smoke-fixture script module: prints a frame, then lingers (no exec,
 # so the process cmdline stays matchable for pgrep).
echo "smoke:ok"
while :; do
    sleep 5
done

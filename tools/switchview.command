#!/bin/sh
# Double-click (or `open -a Terminal tools/switchview.command`) to start
# SwitchView from Terminal, which has the camera permission the capture card
# needs. Reads ../rig.env for the board / fingerbot addresses.
cd "$(dirname "$0")/.." || exit 1
[ -f rig.env ] && . ./rig.env
[ -x app/switchview ] || make || exit 1
exec ./app/switchview "$@" 2>&1 | tee "${TMPDIR:-/tmp}/switchview.log"

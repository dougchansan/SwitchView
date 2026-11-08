#!/bin/sh
# Double-click (or `open -a Terminal tools/switchview.command`) to start
# SwitchView from Terminal, which shows its output live. SwitchView.app
# (tools/make-app.sh) does the same without a terminal; both use launch.sh.
exec "$(dirname "$0")/launch.sh" "$@"

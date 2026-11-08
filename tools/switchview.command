#!/bin/sh
# Double-click (or `open -a Terminal tools/switchview.command`) to start
# SwitchView from Terminal, which has the camera permission the capture card
# needs. Reads ../rig.env for the board / fingerbot addresses.
cd "$(dirname "$0")/.." || exit 1
[ -f rig.env ] && . ./rig.env
[ -x app/switchview ] || make || exit 1

# Power actions need the fingerbot HTTP server (ns2jb-rpc fingerbot/serve.command).
# If SV_FINGERBOT is on this Mac and nothing answers, start it here: it then
# inherits Terminal's Bluetooth permission and outlives this window.
case "$SV_FINGERBOT" in
  http://127.0.0.1:*|http://localhost:*)
    if ! curl -s -m 2 -o /dev/null "$SV_FINGERBOT/"; then
      fb_dir="${SV_FINGERBOT_DIR:-${NS2_DIR:-$HOME/ns2jb-rpc}/fingerbot}"
      fb_port=$(printf '%s\n' "$SV_FINGERBOT" | sed -E 's#^http://[^:]+:([0-9]+).*#\1#')
      if [ -x "$fb_dir/serve.command" ]; then
        echo "fingerbot: starting $fb_dir/serve.command on port $fb_port (log: ${TMPDIR:-/tmp}/fingerbot.log)"
        FINGERBOT_PORT=$fb_port nohup "$fb_dir/serve.command" >"${TMPDIR:-/tmp}/fingerbot.log" 2>&1 &
      else
        echo "fingerbot: nothing at $SV_FINGERBOT and no $fb_dir/serve.command; power actions will fail"
      fi
    fi
    ;;
esac

exec ./app/switchview "$@" 2>&1 | tee "${TMPDIR:-/tmp}/switchview.log"

#!/bin/sh
# Shared start-up for tools/switchview.command (Terminal) and SwitchView.app
# (tools/make-app.sh): reads ../rig.env, builds if needed, starts the fingerbot
# server if it is missing, then runs ./app/switchview. Output goes to the
# terminal and $TMPDIR/switchview.log, or only to the log when there is no
# terminal (launched from Finder / the Dock).
cd "$(dirname "$0")/.." || exit 1
# Finder-launched apps get a bare PATH; make, pkg-config and node live here.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
[ -f rig.env ] && . ./rig.env
LOG="${TMPDIR:-/tmp}/switchview.log"

# Rebuild when sources changed; a failed build still runs the last good binary.
if ! make >"${TMPDIR:-/tmp}/switchview-build.log" 2>&1; then
  echo "switchview: build failed (see ${TMPDIR:-/tmp}/switchview-build.log)"
  [ -x app/switchview ] || exit 1
fi

# Power actions need the fingerbot HTTP server (ns2jb-rpc fingerbot/serve.command).
# If SV_FINGERBOT is on this Mac and nothing answers, start it here: it then
# inherits this app's Bluetooth permission and outlives the window.
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

if [ -t 1 ]; then
  exec ./app/switchview "$@" 2>&1 | tee "$LOG"
else
  exec ./app/switchview "$@" >"$LOG" 2>&1
fi

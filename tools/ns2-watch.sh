#!/bin/sh
# Feed for the panel's NS2 log tab: follows the newest NS2 run folder's run.log,
# whoever started the run (the NS2 Run / NS2 Loop buttons, the control API, or
# `node run.cjs` by hand), and switches to each new run as it appears. Run
# folders live in SwitchView's state dir (NS2_RUNS_DIR) or next to run.cjs.
# It also writes the start/end lines the panel's progress pill keys on, which
# run.log itself does not contain.
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/switchview/ns2/runs"
NS2="${NS2_DIR:-$HOME/ns2jb-rpc}/runs"
cur=""; tp=""; ended=""
trap '[ -n "$tp" ] && kill "$tp" 2>/dev/null' EXIT INT TERM

while :; do
  new=$(ls -td "$STATE"/*/ "$NS2"/*/ 2>/dev/null | head -n 1)
  new=${new%/}
  if [ -n "$new" ] && [ "$new" != "$cur" ] && [ -f "$new/run.log" ]; then
    [ -n "$tp" ] && kill "$tp" 2>/dev/null
    cur=$new; ended=""
    echo
    echo "Waiting for Switch 2 at the host - run $(basename "$cur")"
    tail -n +1 -F "$cur/run.log" 2>/dev/null &
    tp=$!
  fi
  if [ -n "$cur" ] && [ -z "$ended" ] && [ -f "$cur/result.json" ]; then
    sleep 1   # let the tail print run.log's last lines first
    if grep -q '^  "verified": true' "$cur/result.json"; then
      echo "FS execution verified (4/4) - run $(basename "$cur")"
    else
      why=$(sed -n 's/^  "error": "\(.*\)",\{0,1\}$/\1/p' "$cur/result.json" | head -n 1)
      echo "The run stopped - ${why:-no 4/4} - run $(basename "$cur")"
    fi
    ended=1
  fi
  sleep 2
done

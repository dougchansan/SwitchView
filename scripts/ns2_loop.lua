-- NS2 Loop: run NS2 Run (smilebasic_runner.lua) until the host reports 4/4.
-- After any run that ends short of 4/4 ("The run stopped", no connection, a
-- missed keystroke...), hard-restart the console with the fingerbot and run
-- again. Stops on 4/4, on a setup problem a restart cannot fix (host not
-- started), or when you press Stop.
--
--   SV_NS2_LOOP_MAX   give up after this many attempts (default 0 = no limit)

local max = tonumber(os.getenv("SV_NS2_LOOP_MAX") or "") or 0
local src = debug.getinfo(1, "S").source:match("^@(.*)") or ""
local here = src:match("^(.*/)") or "./"   -- this script's folder
local runner = here .. "smilebasic_runner.lua"

local attempt = 0
while true do
  attempt = attempt + 1
  log(string.format("NS2 Loop: attempt %d%s", attempt, max > 0 and (" of " .. max) or ""))
  NS2_OUTCOME = nil
  dofile(runner)
  local outcome = NS2_OUTCOME
  if outcome == "verified" then
    log(string.format("NS2 Loop: 4/4 verified on attempt %d", attempt))
    return
  end
  if outcome == "setup" then
    log("NS2 Loop: stopped - fix the setup problem above, then start the loop again")
    return
  end
  if max > 0 and attempt >= max then
    log(string.format("NS2 Loop: no 4/4 after %d attempts - giving up", attempt))
    return
  end
  log("NS2 Loop: no 4/4 -> Hard Restart, then NS2 Run again")
  power("restart")   -- blocks until the console is back up (or gave up)
end

-- Drive SmileBASIC 4 through the NS2 demo, start to finish.
--
-- The host helper lives in third_party/ns2jb and is started for you: this
-- script spawns it, reads the V=START(...) line it writes to its newest run
-- folder, then drives the console with button presses and typed text.  The
-- host's output is forwarded to the script log, and the script waits for its
-- verdict from result.json without a timeout - press Stop to end it.  The
-- address the console dials comes from the previous run, so there is nothing
-- to edit.
--
-- NS2 must already be saved in slot 0.  Run folders and the host log live
-- under $XDG_STATE_HOME/switchview/ns2 (default ~/.local/state/...), not in
-- the source tree.
--
-- Overrides: NS2_DIR (host folder), NS2_NODE (Node 22+ binary),
-- NS2_HOST (address the console dials), NS2_START (full START line).

local PORT = 39380

rule("error-code", "error code")
rule("loaded", "dist boot ready")
rule("direct", "bytes free")
rule("topmenu", "make a program")
rule("home", "smilebasic")
rule("load-confirm", "load the file")
rule("tag-dialog", "tag information")

-- ---------------- helpers ----------------

local function sh(cmd)
  local p = io.popen(cmd .. " 2>/dev/null")
  if not p then return "" end
  local out = p:read("*a") or ""
  p:close()
  return out
end

local function first_line(cmd)
  local p = io.popen(cmd .. " 2>/dev/null")
  if not p then return nil end
  local line = p:read("*l")
  p:close()
  if line == "" then return nil end
  return line
end

local function read_all(path)
  local f = io.open(path, "r")
  if not f then return nil end
  local body = f:read("*a")
  f:close()
  return body
end

local function read_line(path)
  local body = read_all(path)
  return body and body:match("^([^\n]*)")
end

local function exists(path)
  local f = io.open(path, "r")
  if f then f:close() return true end
  return false
end

local function enter(line)
  type(line .. "\n")
  sleep(0.1)
end

local function dismiss_error()
  if find("error code") then
    tap("A"); sleep(2)
    return true
  end
  return false
end

local function expect(word, timeout)
  local deadline = os.time() + timeout
  while os.time() < deadline do
    dismiss_error()
    if find(word) then return true end
    sleep(0.3)
  end
  return false
end

local function expect_any(a, b, timeout)
  local deadline = os.time() + timeout
  while os.time() < deadline do
    dismiss_error()
    if find(a) then return a end
    if find(b) then return b end
    sleep(0.3)
  end
  return nil
end

-- Back at the Direct-mode prompt: the command just typed has finished when an
-- OK sits directly above the [WORKSPACE] bar (while it runs, the command line
-- itself is the last line). The start-up banner's "bytes free" is no proof:
-- it is only there until a few lines scroll it away or CLS clears it.
local function at_prompt(timeout)
  local deadline = os.time() + timeout
  while os.time() < deadline do
    dismiss_error()
    if ((text() or ""):lower():gsub("%s", "")):find("ok%[?workspace") then return true end
    sleep(0.3)
  end
  return false
end

-- ---------------- host side ----------------

-- The app loads us as <repo>/scripts/<name>.lua, so our own source path is
-- the most reliable way to find the repo; the executable path is a fallback.
local src = ((debug.getinfo(1, "S") or {}).source or ""):gsub("^@", "")
local root = src:match("^(.*)/scripts/[^/]+$")
if not root then
  local exe = first_line("readlink /proc/self/exe") or ""
  root = exe:match("^(.*)/app/[^/]+$") or exe:match("^(.*)/[^/]+$") or "."
end

local hostdir = os.getenv("NS2_DIR")
if not hostdir or hostdir == "" then hostdir = root .. "/third_party/ns2jb" end
hostdir = first_line("realpath '" .. hostdir .. "'") or hostdir

local function find_node()
  local env = os.getenv("NS2_NODE")
  if env and env ~= "" then return env end
  local found = first_line("command -v node")
  if found then return found end
  for _, guess in ipairs{root .. "/tools/node", root .. "/../tools/node"} do
    if exists(guess) then return guess end
  end
  return nil
end

local function runs_dir()
  local base = os.getenv("XDG_STATE_HOME")
  if not base or base == "" then
    local home = os.getenv("HOME")
    if not home then return nil end
    base = home .. "/.local/state"
  end
  return base .. "/switchview/ns2/runs"
end

local RUNS = runs_dir()
local HOST_LOG = RUNS and RUNS:gsub("/runs$", "") .. "/host.log" or "/tmp/switchview-ns2.log"

-- Run folders, newest first. The host is asked to use RUNS (NS2_RUNS_DIR), but
-- some versions always write to runs/ next to run.cjs, so look in both.
local function run_files(name)
  local globs = {}
  for _, d in ipairs{RUNS, hostdir .. "/runs"} do
    if d then globs[#globs + 1] = '"' .. d .. '"/switch2-*/' .. name end
  end
  return io.popen("ls -1t " .. table.concat(globs, " ") .. " 2>/dev/null")
end

-- The address the console dials: NS2_HOST, else the one the last run used
-- (the host records it in connect.txt), else the shared-link default.
local function last_host()
  local p = run_files("connect.txt")
  local path = p and p:read("*l")
  if p then p:close() end
  local line = path and read_line(path)
  if not line then return nil end
  local a, b, c, d = line:match("^V=START%(%d+,(%d+),(%d+),(%d+),(%d+)%)")
  if not a then return nil end
  return table.concat({a, b, c, d}, ".")
end

-- Only reuse an address this PC still has; a stale one from another network
-- would make the host fail to bind. `ip` on Linux, `ifconfig` on macOS.
local function local_ip(ip)
  local out = sh("ip -4 -o addr show scope global 2>/dev/null; ifconfig 2>/dev/null")
  return out:find("inet " .. ip .. "/", 1, true) ~= nil or out:find("inet " .. ip .. " ", 1, true) ~= nil
end

-- This PC's address on the network its default route uses: the one a console
-- on the same LAN can reach.
local function default_ip()
  local ip = first_line("ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \\([0-9.]*\\).*/\\1/p'")
  if ip and ip ~= "" then return ip end
  local ifc = first_line("route -n get default 2>/dev/null | awk '/interface:/{print $2}'")
  if ifc and ifc ~= "" then ip = first_line("ipconfig getifaddr " .. ifc .. " 2>/dev/null") end
  if ip and ip ~= "" then return ip end
  return nil
end

local HOST = os.getenv("NS2_HOST")
if not HOST or HOST == "" then
  HOST = last_host()
  if not HOST or not local_ip(HOST) then HOST = default_ip() or "10.42.0.1" end
end

-- Forward new lines from the host's own output into the script log, so the
-- 1/4 .. 4/4 progress is visible while the script waits.
local owns_host = false
local host_pid
local log_offset = 0
local function tail_host_log()
  if not owns_host or not HOST_LOG then return end
  local f = io.open(HOST_LOG, "r")
  if not f then return end
  f:seek("set", log_offset)
  local chunk = f:read("*a") or ""
  f:close()
  if chunk == "" then return end
  log_offset = log_offset + #chunk
  for line in chunk:gmatch("[^\r\n]+") do
    local trimmed = line:gsub("^%s+", "")
    -- Same style as our own lines: no trailing full stop.
    if trimmed:sub(-1) == "." and trimmed:sub(-2) ~= ".." then
      trimmed = trimmed:sub(1, -2)
    end
    -- Skip the host's printed instructions; keep status and stage lines.
    local banner = line:match("^%s%s") or trimmed:match("^In a freshly opened")
      or trimmed:match("^After the expected") or trimmed:match("^If BASIC says")
      or trimmed:match("^Run folder:")
    if trimmed ~= "" and not banner then log(trimmed) end
  end
end

-- Run folders that existed before this run's host started; a stale one
-- without result.json must not be mistaken for the new run.
local old_runs = {}
local function remember_old_runs()
  old_runs = {}
  local p = run_files("connect.txt")
  if not p then return end
  for line in p:lines() do old_runs[line] = true end
  p:close()
end

-- Newest run folder that has not finished yet (result.json absent).
local function waiting_run()
  local p = run_files("connect.txt")
  if not p then return nil end
  local found
  for line in p:lines() do
    local dir = (line:gsub("/connect%.txt$", ""))
    if not old_runs[line] and not exists(dir .. "/result.json") then found = dir; break end
  end
  p:close()
  return found
end

-- Linux (ss, /proc) first, macOS (lsof, kill -0) as the fallback.
local function listener_pid()
  local pid = tonumber(sh("ss -Hltnp 'sport = :" .. PORT .. "' 2>/dev/null"):match("pid=(%d+)"))
  return pid or tonumber(first_line("lsof -nP -iTCP:" .. PORT .. " -sTCP:LISTEN -t 2>/dev/null") or "")
end

local function alive(pid)
  if pid == nil then return false end
  if first_line("readlink /proc/" .. pid .. "/exe 2>/dev/null") ~= nil then return true end
  return os.execute("kill -0 " .. pid .. " 2>/dev/null") == true
end

-- True when a listener is the copy this script starts (its working directory
-- is the host folder).
local function our_host(pid)
  local cwd = first_line("readlink /proc/" .. pid .. "/cwd 2>/dev/null")
  if not cwd then cwd = first_line("lsof -a -p " .. pid .. " -d cwd -Fn 2>/dev/null | sed -n 's/^n//p'") end
  return cwd == hostdir
end

-- Stop the host this script started, or a leftover copy from an earlier run.
-- A listener belonging to something else is left alone.
local function despawn_host()
  local pid = host_pid
  if not alive(pid) then
    pid = listener_pid()
    if not pid then host_pid = nil; return true end
    if not our_host(pid) then
      log("Port " .. PORT .. " in use by another program - left alone")
      return false
    end
  end
  os.execute("kill -TERM " .. pid)
  for _ = 1, 20 do
    if not alive(pid) then host_pid = nil; return true end
    sleep(0.2)
  end
  os.execute("kill -KILL " .. pid)
  sleep(0.2)
  host_pid = nil
  return not alive(pid)
end

local function start_host(node)
  if not RUNS then return false end
  log_offset = 0                 -- the redirect below truncates host.log
  os.execute("mkdir -p '" .. RUNS .. "'")
  local cmd = string.format(
    "cd '%s' && NS2_RUNS_DIR='%s' nohup '%s' run.cjs '%s' >'%s' 2>&1 & echo $!",
    hostdir, RUNS, node, HOST, HOST_LOG)
  local p = io.popen(cmd)
  host_pid = p and tonumber(p:read("*l"))
  if p then p:close() end
  log("Host started: " .. HOST)
  return true
end

-- Fresh host for this run: stop a leftover copy, start one, wait for its run
-- folder.
local function ensure_host()
  if listener_pid() and not despawn_host() then return nil end
  local node = find_node()
  if not node then
    log("Node 22+ not found - set NS2_NODE")
    return nil
  end
  remember_old_runs()
  if not start_host(node) then return nil end
  local deadline = os.time() + 15
  while os.time() < deadline do
    sleep(0.5)
    local dir = waiting_run()
    if dir then return dir end
  end
  log("Host opened no run folder - see " .. HOST_LOG)
  return nil
end

-- ---------------- console side ----------------

-- HOME menu, "Make a Program", or Direct mode -> Direct mode.  Dialogs are
-- re-checked in the loop because "Tag Information" can follow the launch.
local have_nav, nav = pcall(require, "nav")

local function to_direct()
  -- The shared navigator gets there from any screen (sleep, other games,
  -- dialogs, a wedged program); the loop below is the old fallback.
  if have_nav then
    local ok, state = nav.go("smilebasic")
    if ok then return state == "loaded" and "loaded" or "direct" end
    return nil
  end
  local deadline = os.time() + 30
  local launched = false
  while os.time() < deadline do
    if dismiss_error() then launched = false end
    if find("dist boot ready") then return "loaded" end
    if find("bytes free") then return "direct" end
    if find("tag information") or find("make a program") then
      tap("A"); sleep(1.5)
    elseif find("smilebasic") and not launched then
      tap("A"); sleep(6)                                 -- launch from HOME
      launched = true
    else
      sleep(0.5)
    end
  end
  return nil
end

-- ---------------- run ----------------

local function give_up(msg)
  tail_host_log()
  log(msg)
  if owns_host then despawn_host() end
end

-- An explicit NS2_START means a host we did not start: never touch it.
local run_dir
local start = os.getenv("NS2_START")
if start and start ~= "" then
  run_dir = waiting_run()
else
  run_dir = ensure_host()
  owns_host = run_dir ~= nil
  if not run_dir then
    give_up("Host not started - console untouched (check NS2_NODE / NS2_DIR)")
    return
  end
  local line = read_line(run_dir .. "/connect.txt")
  if line and line:match("^V=START%(") then
    start = line
  else
    start = "V=START(" .. PORT .. "," .. (HOST:gsub("%.", ",")) .. ")"
  end
end
-- Always type the nine-argument form; the host prints the five-argument one,
-- which newer bootstraps reject with "Wrong number of arguments".
do
  local args = start:match("^V=START%(([%d,]+)%)%s*$")
  if args and select(2, args:gsub(",", "")) == 4 then
    start = "V=START(" .. args .. ",0,0,0,0)"
  end
end
tail_host_log()

local state = to_direct()
if not state then
  give_up("Not in SmileBASIC Direct mode - open Make a Program and retry")
  return
end
if state == "loaded" then
  give_up("Bootstrap already ran - restart SmileBASIC to start over")
  return
end

enter('LOAD "TXT:NS2"')
if expect("load the file", 8) then
  tap("A")
  sleep(1.5)
end
if not at_prompt(20) then
  give_up("LOAD failed - not back in Direct mode")
  return
end
log("NS2 loaded")

enter("RUN")
if not expect("dist boot ready", 25) then
  give_up("No DIST BOOT READY - is NS2 the current program?")
  return
end
log("Bootstrap ran")

-- Expected "Out of memory"; the demo's REMOVE is deliberately oversized.
enter("REMOVE NSR,&H60000000,&H3FFFFFFF")
log("REMOVE issued")

-- "DIST BOOT READY" stays on screen, so the screen is still labelled
-- "loaded"; the status line is what proves we are back at the prompt.
if not at_prompt(10) then
  give_up("REMOVE failed - not back in Direct mode")
  return
end

enter(start)
local outcome = expect_any("host connected", "boot failed", 10)
if outcome == "boot failed" then
  give_up("BOOT FAILED - check the host address, then restart SmileBASIC")
  return
elseif outcome == "host connected" then
  log("Console connected")
end

if not run_dir then
  log("No run folder to watch")
  return
end

-- Wait for the host's verdict, forwarding its progress as it happens.  No
-- timeout: the run can take as long as it takes, and Stop ends the script.
while true do
  tail_host_log()
  local result = read_all(run_dir .. "/result.json")
  if result then
    tail_host_log()
    if result:find('"verified": true', 1, true) then
      log("Verified - instructions restored, restart the console")
    else
      log("Host reported: " .. (result:match('"error":%s*"([^"]*)"') or "run failed"))
      log("Details: " .. run_dir)
    end
    if owns_host then despawn_host() end   -- normally already exited
    return
  end
  -- A host we started whose process is gone has died without a result.
  -- (Its listening socket closes as soon as the console pairs, so the
  -- listener disappearing is normal and not used here.)
  if owns_host and not alive(host_pid) then
    tail_host_log()
    log("Host exited without a result - see " .. HOST_LOG)
    return
  end
  sleep(1)
end

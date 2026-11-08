-- nav: get the Switch 2 from whatever it is showing to a target screen, fast.
--
--   local nav = require "nav"
--   nav.go("smilebasic")      -- SmileBASIC 4 (retail) Direct mode prompt
--   nav.go("seedsow")         -- Seedsow Lullaby running
--   nav.home()                -- HOME menu
--
-- This is the ns2-savekit harness (harness.cjs launchSmileBASIC +
-- console-state.cjs) moved inside SwitchView. What makes it quick:
--   * the frame is already in memory and OCR runs in-process (Apple Vision on
--     macOS), so a look costs ~0.1 s instead of an ffmpeg + OCR spawn;
--   * waits are tied to the picture - mark()/changed()/settle() compare 64x36
--     fingerprints - instead of fixed 1.5-12 s sleeps;
--   * screens you teach with learn("<state>") are recognised by fingerprint
--     alone, with no OCR at all (see README "Teaching screens").
--
-- Every step logs "state -> action", so a stuck run shows what it saw.

local nav = {}

-- Selected tile's title on the HOME row (0..1 from the top). Measured from
-- the harness's Vision band 0.70-0.82 (bottom origin).
nav.TITLE_BAND = {0.12, 0.34}
-- Tiles we know by name; any of these in the title band means "HOME row".
nav.TILES = {"smilebasic", "seedsow", "lullaby", "mario kart", "sports resort", "nora",
             "pokemon", "pokémon", "wagamama", "zelda", "splatoon", "kirby", "donkey kong"}
nav.MATCH_MAX = 6        -- learned-screen distance that counts as "this screen"
nav.verbose = true

-- When the RPC is wedged ("rpc running" with no prompt) the fix is to close and
-- relaunch the software. If that keeps failing - the screen never comes back, or
-- each relaunch comes up wedged again - escalate to a hard console reboot (force
-- off + boot). Tunable; set SV_WEDGE_HARD_REBOOT=0 to keep the old soft-only loop.
nav.WEDGE_HARD_REBOOT = os.getenv("SV_WEDGE_HARD_REBOOT") ~= "0"
nav.WEDGE_LOOKS = tonumber(os.getenv("SV_WEDGE_LOOKS") or "6")   -- wedged looks in a row -> reboot
nav.WEDGE_FRESHES = tonumber(os.getenv("SV_WEDGE_FRESHES") or "3") -- soft relaunches -> reboot
nav.WEDGE_REBOOTS = tonumber(os.getenv("SV_WEDGE_REBOOTS") or "2") -- hard reboots before giving up

local function say(...) if nav.verbose then log(...) end end
local function has(t, s) return t:find((s:gsub("%s", "")), 1, true) ~= nil end
local function word(t, w) return t:find("%f[%w]" .. w .. "%f[%W]") ~= nil end

-- ---------------------------------------------------------------- looking

-- Text -> state. Order matters: the most specific / most dangerous first.
function nav.classify(t, title)
  t = t or ""
  -- OCR splits and joins words unpredictably ("control ler"), so phrases are
  -- matched with every space removed from both sides.
  local q = t:gsub("%s", "")
  if has(q, "2162-0002") or has(q, "error code") or has(q, "an error has occurred") then return "error-dialog" end
  if has(q, "rpc running") and not has(q, "rpc stop") then return "wedged-rpc" end
  if has(q, "dist boot ready") then return "loaded" end
  -- Seedsow Lullaby (Demo) save launcher and its HTTP server mode.
  if has(q, "[httpd]") or has(q, "control channel ready") or (has(q, "http server") and has(q, "listen")) then
    return "http-console"
  end
  if has(q, "artemis-launcher") or has(q, "press a button on the controller to launch") then
    return "artemis-launcher"
  end
  if has(q, "bytes free") and word(t, "ok") then return "direct-mode" end
  if has(q, "rpc stop") and word(t, "ok") then return "direct-mode" end
  if has(q, "are you sure") or has(q, "load the file") then return "sb-confirm" end
  if has(q, "three times") then return "lock-screen" end
  if has(q, "close the software") or has(q, "unsaved data") or has(q, "will be closed")
     or (has(q, "close") and has(q, "software") and has(q, "cancel")) then return "close-dialog" end
  if has(q, "who is using") or has(q, "who is playing") or has(q, "who's playing") or has(q, "select a user")
     or has(q, "select user") or has(q, "choose a user") then return "user-select" end
  if has(q, "tag information") then return "tag-dialog" end
  if has(q, "add user") or has(q, "nintendo account") then return "add-user" end
  if has(q, "playmenu") then return "playmenu" end
  if has(q, "topmenu") or has(q, "make a program") then return "topmenu" end
  -- The wake/start screen: "Featured News" on the left, "(A) Start" in the middle.
  if has(q, "featured news") then return "home-news" end
  title = title or ""
  -- HOME: the clock leads the reading order (games don't draw it) and the
  -- selected tile's name sits above the row; the footer helps on transitions.
  local clock = t:match("^%d%d?:%d%d") ~= nil or t:match("^%d%d?:%s?%d%d") ~= nil
  if clock and (title ~= "" or has(q, "close software") or has(q, "options")) then return "home-row" end
  for _, name in ipairs(nav.TILES) do
    if has((title:gsub("%s", "")), name) then return "home-row" end
  end
  -- System UI we have no rule for has a "(B) Back  (A) OK" footer: back out.
  if word(t, "back") and word(t, "ok") then return "system-dialog" end
  return "unknown"
end

-- One look at the console: state, full text, selected-tile title.
-- Fingerprints first (free), then OCR.
function nav.look()
  local dark, mean = flat()
  if dark then return "dark", "", "" end
  local m, d = match()
  if m and d <= nav.MATCH_MAX then return m, "", "" end
  local t = text()
  local title = band(nav.TITLE_BAND[1], nav.TITLE_BAND[2])
  return nav.classify(t, title), t, title
end

-- ---------------------------------------------------------------- acting

-- Press and wait for the picture to react, then for it to stop moving.
function nav.press(btn, hold)
  mark()
  tap(btn, hold or 0.1)
  changed(1.5)
  settle(1.2)
end

function nav.step(dir)
  mark()
  dpad(dir, 0.08)
  changed(0.9)
  settle(0.8)
end

-- Wait until the screen is one of the wanted states (a set or a single name).
function nav.wait(wanted, timeout)
  if luatype(wanted) == "string" then wanted = {[wanted] = true} end  -- type() is the keyboard
  local deadline = now() + (timeout or 10)
  local s, t, title
  repeat
    s, t, title = nav.look()
    if wanted[s] then return s, t, title end
    settle(0.6)
  until now() > deadline
  return nil, s, t, title
end

-- Dark screen: transition, sleep, or off. Wait briefly, wake with the pad,
-- then the fingerbot as a last resort.
function nav.wake()
  local t0 = now()
  local pressed_home, booted = false, false
  while true do
    if not flat() then return true end
    local dt = now() - t0
    if dt > 4 and not pressed_home then
      say("dark for 4 s -> HOME (wakes a sleeping console)")
      tap("HOME"); pressed_home = true
    elseif dt > 12 and not booted then
      local ok = pcall(power, "toggle")
      if ok then say("still dark -> fingerbot wake press") end
      booted = true
      t0 = now() - 12.5
    elseif dt > 30 then
      local ok, ran = pcall(power, "boot")
      if not ok or not ran then say("no picture and no fingerbot to boot it"); return false end
      say("booted with the fingerbot")
      if not flat() then return true end
      return false
    end
    sleep(0.5)
  end
end

-- HOME menu with the tile row showing.
function nav.home()
  for attempt = 1, 4 do
    local s = nav.look()
    if s == "home-row" then return true end
    if s == "dark" then nav.wake()
    elseif s == "lock-screen" then
      say("lock screen -> A A A"); tap("A"); tap("A"); tap("A"); settle(1.5)
    elseif s == "home-news" then
      say("start screen -> A"); nav.press("A")
    else
      say(s .. " -> HOME"); nav.press("HOME")
    end
    if nav.wait("home-row", 2.5) then return true end
  end
  return false
end

-- Select the tile whose title matches `want` (and not `avoid`) on the HOME row.
-- Returns true, or false and the state that interrupted it (a dialog, etc.).
function nav.select_tile(want, avoid)
  local function hit(title)
    return title:find(want) and not (avoid and title:find(avoid))
  end
  local s, _, t = nav.look()
  if s ~= "home-row" then return false, s end
  if hit(t) then return true end
  -- The row reorders by recent use, so the target is usually near the left.
  -- Jump to the first tile (hold left), then walk right reading each title.
  say("tile '" .. t .. "' -> first tile")
  mark(); dpad("left", 1.4); settle(0.8)
  for i = 1, 24 do
    s, _, t = nav.look()
    if s ~= "home-row" then return false, s end
    if hit(t) then say("selected '" .. t .. "'"); return true end
    nav.step("right")
  end
  say("never found a tile matching '" .. want .. "' (last '" .. t .. "')")
  return false, "not-found"
end

-- Account picker: go to the first user, then SV_USER_INDEX-1 to the right.
function nav.pick_user()
  local idx = tonumber(os.getenv("SV_USER_INDEX") or "1") or 1
  for i = 1, 4 do dpad("left", 0.06); sleep(0.08) end
  for i = 2, idx do nav.step("right") end
  settle(0.5)
  nav.press("A")
end

-- Close the selected (running) software from the HOME row.
function nav.close_selected()
  say("closing the running software (X, then confirm)")
  nav.press("X")
  local s = nav.wait({["close-dialog"] = true}, 3)
  if s then nav.press("A") end
  settle(2)
end

-- ---------------------------------------------------------------- targets

-- A target is: how to find its tile, which states mean "done", and what to
-- do on each state while getting there. Unknown states fall back to HOME.
nav.targets = {}

nav.targets.smilebasic = {
  tile = "smilebasic", avoid = "demo",            -- retail, not the Demo tile
  done = {["direct-mode"] = true, ["loaded"] = true},
  splash_a = true,          -- after launch, A through splash/logo screens (harness-proven)
  on = {
    ["topmenu"] = function() nav.press("A") end,           -- Make a Program -> Direct mode
    ["playmenu"] = function() nav.press("B") end,
    ["tag-dialog"] = function() nav.press("A") end,
    ["user-select"] = function() nav.pick_user() end,
    ["sb-confirm"] = function() nav.press("B") end,       -- a half-typed LOAD: cancel it
    ["error-dialog"] = function() nav.press("A") end,
  },
  -- A wedged RPC program never gives the prompt back: close and relaunch.
  fresh_on = {["wedged-rpc"] = true},
}

nav.targets.seedsow = {
  tile = os.getenv("SV_SEEDSOW_TILE") or "seedsow",
  avoid = os.getenv("SV_SEEDSOW_AVOID"),             -- e.g. "demo" to skip the demo tile
  -- The save launcher menu (or its HTTP server, already running) means
  -- Seedsow is up; "seedsow" is a learned screen if you teach one.
  done = {["artemis-launcher"] = true, ["http-console"] = true, ["seedsow"] = true},
  -- Without a taught screen, "done" = the game took over after the launch.
  launched_done = true,
  on = {
    ["user-select"] = function() nav.pick_user() end,
    ["error-dialog"] = function() nav.press("A") end,
  },
}

-- Seedsow's save launcher -> Y: HTTP server ("http control channel ready").
nav.targets.console = {
  tile = nav.targets.seedsow.tile, avoid = nav.targets.seedsow.avoid,
  done = {["http-console"] = true},
  on = {
    ["artemis-launcher"] = function() nav.press("Y") end,
    ["user-select"] = function() nav.pick_user() end,
    ["error-dialog"] = function() nav.press("A") end,
  },
  launch_wait = 40,          -- the save exploit takes a while to show its menu
}

-- nav.go(name) -> true and the final state, or false and a reason.
function nav.go(name, opts)
  opts = opts or {}
  local T = nav.targets[name]
  if not T then return false, "no target " .. tostring(name) end
  local t0 = now()
  local launched, resumed, fresh = false, 0, opts.fresh
  local launched_at = 0
  local waited = false
  local wedged_looks, fresh_count, reboots = 0, 0, 0

  -- Hard console reboot (force off + boot), the escalation for a stuck RPC.
  local function hard_reboot()
    say("rpc wedged -> HARD REBOOT (force off + boot)")
    local ok, ran = pcall(function() return power("restart") end)
    if not ok or not ran then
      say("hard reboot unavailable (no fingerbot configured?)")
      return false
    end
    settle(2)
    return true
  end

  for step = 1, opts.steps or 40 do
    local s, txt, title = nav.look()
    if s ~= "wedged-rpc" then wedged_looks = 0 end
    if T.done[s] then
      say(string.format("%s reached (%s) in %.1f s", name, s, now() - t0))
      return true, s
    end
    if launched and T.launched_done and s == "unknown" and now() - launched_at > 2 then
      -- No taught screen for this game: it is "running" once the launch has
      -- left every system screen behind and stays that way.
      settle(1.5)
      local s2 = nav.look()
      if s2 == "unknown" then
        say(string.format("%s running (%s) in %.1f s", name, s2, now() - t0))
        return true, s2
      end
    end
    if T.fresh_on and T.fresh_on[s] then
      wedged_looks = wedged_looks + 1
      -- Escalate when the soft fix isn't working: the wedged screen won't clear
      -- (many looks in a row) or every relaunch comes back wedged.
      local stuck = wedged_looks >= nav.WEDGE_LOOKS or fresh_count >= nav.WEDGE_FRESHES
      if nav.WEDGE_HARD_REBOOT and stuck and reboots < nav.WEDGE_REBOOTS then
        reboots = reboots + 1
        wedged_looks, fresh_count = 0, 0
        if hard_reboot() then launched, launched_at, fresh = false, 0, false end
      elseif stuck and reboots >= nav.WEDGE_REBOOTS then
        return false, "rpc wedged; hard reboot did not clear it"
      else
        say(s .. " -> needs a fresh start of the software")
        fresh = true
        nav.press("HOME")
      end
    elseif T.on[s] then
      say(s .. " -> handled")
      T.on[s]()
      -- Picking a user (or closing the old software) starts the launch.
      if s == "user-select" and not launched then launched, launched_at = true, now() end
    elseif s == "dark" then
      if not nav.wake() then return false, "no picture" end
    elseif s == "lock-screen" then
      say("lock screen -> A A A"); tap("A"); tap("A"); tap("A"); settle(1.5)
    elseif s == "close-dialog" then
      say("close dialog -> A"); nav.press("A")
      launched, launched_at = true, now()
    elseif s == "home-news" then
      say("start screen -> A"); nav.press("A")
    elseif s == "add-user" or s == "system-dialog" then
      say(s .. " -> B (back out)"); nav.press("B")
    elseif s == "home-row" and launched and now() - launched_at < 8 then
      -- Right after a launch HOME still shows while dialogs slide in.
      settle(1.0)
    elseif s == "home-row" then
      local ok, why = nav.select_tile(T.tile, T.avoid)
      if ok and fresh then
        nav.close_selected(); fresh = false
        fresh_count = fresh_count + 1
        ok, why = nav.select_tile(T.tile, T.avoid)
      end
      if not ok then
        if why == "not-found" then return false, "tile not found" end
        goto continue   -- a dialog appeared: handle it on the next look
      end
      say("launch " .. name .. " (A)")
      mark(); tap("A", 0.12)
      changed(4); settle(3)
      launched, launched_at = true, now()
      goto continue
    elseif launched and now() - launched_at < (T.launch_wait or 30) then
      -- Loading / logo screens right after a launch: let them play; if one
      -- just sits there, A moves it along (what the harness did).
      mark()
      if not changed(2.5) then
        if T.splash_a then say(s .. " after launch (static) -> A"); nav.press("A")
        elseif not waited then say(s .. " after launch: waiting for the software"); waited = true end
      end
    else
      -- Somewhere we have no rule for (inside another game, an editor...).
      -- HOME, reselect, resume; if resuming lands here again, start fresh.
      if launched then resumed = resumed + 1 end
      if resumed >= 2 then fresh = true; resumed = 0 end
      say((s == "unknown" and ("unknown screen '" .. (txt or ""):sub(1, 50) .. "'") or s) .. " -> HOME")
      launched = false
      nav.press("HOME")
      nav.wait("home-row", 3)
    end
    ::continue::
  end
  return false, "gave up after " .. (opts.steps or 40) .. " steps"
end

return nav

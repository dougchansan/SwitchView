-- From any screen to SmileBASIC 4's Direct mode prompt (retail tile).
-- Handles sleep/dark, the lock screen, other games, dialogs, TOPMENU and a
-- wedged RPC program (close + relaunch). See scripts/lib/nav.lua.
local nav = require "nav"
local ok, why = nav.go("smilebasic")
if not ok then log("could not reach SmileBASIC Direct mode: " .. tostring(why)) end

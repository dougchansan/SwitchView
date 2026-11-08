-- From any screen to the HTTP console: Seedsow Lullaby's save launcher
-- (switch-artemis-launcher), then Y = HTTP server. Done when the console
-- prints "http control channel ready". See scripts/lib/nav.lua.
local nav = require "nav"
local ok, why = nav.go("console")
if not ok then log("could not reach the HTTP console: " .. tostring(why)) end

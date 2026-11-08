-- From any screen to Seedsow Lullaby running.
-- SV_SEEDSOW_TILE (Lua pattern, default "seedsow") picks the tile and
-- SV_SEEDSOW_AVOID (e.g. "demo") skips one. Teach the in-game screen once with
--   curl 'http://127.0.0.1:39390/learn?label=seedsow'
-- and arrival is then recognised instantly by its fingerprint.
local nav = require "nav"
local ok, why = nav.go("seedsow")
if not ok then log("could not reach Seedsow Lullaby: " .. tostring(why)) end

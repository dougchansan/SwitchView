-- Fast Restart: bring up the in-game power menu (hold the power button), then
-- Down, A, A to pick Restart and confirm. Clean and quick -- use it when the
-- console is responsive. If it's wedged (pad not polled), use Hard Restart,
-- which forces the console off and boots it without needing the menu.
--
-- Timings are tunable:
--   SV_FR_MENU_S   seconds to wait for the power menu to appear (default 1.2)
--   SV_FR_STEP_S   seconds between Down / A / A (default 0.45)

local menu_s = tonumber(os.getenv("SV_FR_MENU_S") or "1.2")
local step_s = tonumber(os.getenv("SV_FR_STEP_S") or "0.45")

log("Fast Restart: opening the power menu")
power("menu")            -- holds the power button; the menu slides in after release
sleep(menu_s)

dpad("down", 0.12)       -- highlight Restart
sleep(step_s)
tap("A")                 -- select Restart
sleep(step_s)
tap("A")                 -- confirm
log("Fast Restart: Restart selected")

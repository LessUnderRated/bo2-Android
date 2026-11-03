-- The menu appeared then vanished after ~1.5s: the first version jumped straight
-- to the main menu event, but the game's own lock-out handler does more than
-- that before it closes (among other things it tells the engine, through a menu
-- response, that the lock-out is over). Skipped, the engine still believes it is
-- locked out and takes the screen back. So nothing here imitates that handler:
-- the script delivers save_device_selected to the LockOutMenu element, where the
-- game registers its handler, exactly as a real save-device selection would.
local rep = string and string.rep
local function b(n) pcall(function() if rep then local _ = rep("x", 0xB0000 + n) end end) end
b(1)
local LUI = rawget(_G, "LUI")
local CoD = rawget(_G, "CoD")
local Dvar = rawget(_G, "Dvar")
pcall(function() if CoD and CoD.ShadesHud then CoD.ShadesHud.bootupComplete = true end end)
-- P805 : true, comme une vraie sélection de périphérique. `false` envoyait au MainMenu sans popup, mais disait au
-- Lua « pas de sauvegarde » alors que le moteur a le périphérique (byte[0x20c01a1]=1) : NEW STORY affichait
-- « not connected to a save device » et RESUME STORY manquait (vu par le testeur le 2026-10-05).
pcall(function() Dvar.sv_saveDeviceAvailable:set(true) end)
b(3)

-- The LockOutMenu element is the one carrying PressStartButton -- a unique
-- marker, no name guessing.
local lockout = nil
local function walk(e, d)
  if e == nil or d > 14 or lockout then return end
  local hit = nil
  pcall(function() hit = e.PressStartButton end)
  if hit ~= nil then lockout = e; return end
  local c = nil
  pcall(function() c = e:getFirstChild() end)
  while c ~= nil and not lockout do
    walk(c, d + 1)
    local nx = nil
    pcall(function() nx = c:getNextSibling() end)
    c = nx
  end
end
pcall(function() for _, r in pairs(LUI.roots) do walk(r, 0) end end)
b(lockout and 4 or 5)

if lockout then
  local ok = pcall(function()
    lockout:processEvent({ name = "save_device_selected", controller = 0 })
  end)
  b(ok and 6 or 7)
end
b(2)
-- Usage:
--   bash scripts/menu_run.sh /tmp/out BO2_LUA_SRC=$PWD/scripts/lua/open_main_menu.lua \
--        BO2_LUA_WATCH=1 BO2_UIX_PROBE=1
-- BO2_UIX_PROBE is needed by the runner to confirm LockOutMenu is actually open
-- before the key goes out; without it the key can land too early and the test
-- means nothing.

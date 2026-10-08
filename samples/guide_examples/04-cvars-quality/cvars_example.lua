-- Глава 04: cvar'ы из Lua через API-модуль `cvar`, который зарегистрировал C++ (см. lua_cvars.cpp).
densityBefore = cvar.get("fx.Grass.Density")     -- "1": значения приходят строками
cvar.set("ui.ShowFps", "true")
if tonumber(densityBefore) > 0.5 then
    cvar.exec("fx.Grass.Density 0.5")            -- та же строка, что и в консоли
end
unknownSet = cvar.set("no.such.cvar", "1")       -- false
log.info("grass density is now", cvar.get("fx.Grass.Density"))

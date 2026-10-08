-- Отражения: [R] SSR on/off, [G] GTAO on/off, [B] re-captures the reflection probes (OnEnable probes).
local flow = require("lib.flow")
properties = { mode = { type = "string", default = "reflections" } }

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    if input.keyPressed("R") then
        local on = showcase.cvar("r.SSR") == "true"
        showcase.setCVar("r.SSR", not on)
        flow.toast(on and "SSR выключен: отражения из проб" or "SSR включён")
    end
    if input.keyPressed("G") then
        local m = tonumber(showcase.cvar("r.AO.Method") or "2")
        showcase.setCVar("r.AO.Method", m == 0 and 2 or 0)
        flow.toast(m == 0 and "GTAO включён" or "AO выключен")
    end
end

-- 256 coloured point lights (clustered forward+): [L] animates them as a wave, [P] toggles PCSS soft shadows.
local flow = require("lib.flow")

function onStart(self)
    self.lights = {}
    for _, c in ipairs(self.entity:children()) do
        local l = c:get("Light")
        if l then self.lights[#self.lights + 1] = { light = l, base = c.transform.position, color = l.color } end
    end
    self.animate = true
    self.t = 0
end

function onUpdate(self, dt)
    if showcase.mode() == "play" and not flow.blocking() then
        if input.keyPressed("L") then
            self.animate = not self.animate
            flow.toast(self.animate and "Гирлянда: волна" or "Гирлянда: статично")
        end
        if input.keyPressed("P") then
            local on = showcase.cvar("r.Shadows.PCSS") == "true"
            showcase.setCVar("r.Shadows.PCSS", not on)
            flow.toast(on and "PCSS выключен: жёсткие тени" or "PCSS включён: контактное упрочнение теней")
        end
    end
    if not self.animate then return end
    self.t = self.t + dt
    for i, l in ipairs(self.lights) do
        local p = l.base
        local wave = 0.5 + 0.5 * math.sin(self.t * 2.0 - (p.x * 0.35 + p.z * 0.9))
        l.light.intensity = 30 + 140 * wave
    end
end

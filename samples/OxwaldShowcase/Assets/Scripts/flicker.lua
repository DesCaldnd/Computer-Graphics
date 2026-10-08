-- Campfire light flicker.
properties = { light = { type = "string", default = "FireLight" }, base = { type = "float", default = 5000 } }

function onStart(self)
    local e = scene.find(self.light)
    self.l = e and e:get("Light")
    self.t = 0
end

function onUpdate(self, dt)
    if not self.l then return end
    self.t = self.t + dt
    local n = math.sin(self.t * 13.1) * 0.5 + math.sin(self.t * 7.3 + 1.7) * 0.3 + math.sin(self.t * 23.9) * 0.2
    self.l.intensity = self.base * (0.85 + 0.15 * n)
end

-- Reverb zone: the tunnel trigger crossfades the wet level of the "Cave" bus reverb.
local flow = require("lib.flow")
properties = { wet = { type = "float", default = 0.55 } }

function onStart(self) self.current = 0; self.target = 0 end

function onTriggerEnter(self, other)
    if other.name == "Player" then self.target = self.wet; flow.toast("Зона реверберации: шина Cave") end
end

function onTriggerExit(self, other)
    if other.name == "Player" then self.target = 0 end
end

function onUpdate(self, dt)
    if math.abs(self.current - self.target) > 0.01 then
        self.current = math.lerp(self.current, self.target, math.min(1, dt * 3))
        showcase.audio.reverb("Cave", self.current, 0.9)
    end
end

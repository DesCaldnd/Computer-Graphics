-- Lamp switch: toggles the Light of the linked lamp entity (that entity has SaveGame: its state is saved).
local interact = require("lib.interact")
properties = { lamp = { type = "string", default = "", tooltip = "Lamp entity UUID" }, on = { type = "float", default = 2500 } }

function onStart(self) self.light = scene.find(self.lamp) end

function onUpdate(self, dt)
    if not self.light then return end
    interact.update(self, 1.8, "Переключить лампу", function()
        local l = self.light:get("Light")
        l.intensity = l.intensity > 0 and 0 or self.on
        audio.play("project://Assets/Audio/beep.wav", self.entity.transform.worldPosition, 0.4)
    end)
end

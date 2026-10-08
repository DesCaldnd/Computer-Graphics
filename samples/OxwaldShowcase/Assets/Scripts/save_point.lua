-- Save point: [E] writes the station into its own slot (SaveGameSystem; crates/lamps with SaveGame are persisted).
local interact = require("lib.interact")
local flow = require("lib.flow")
properties = { slot = { type = "string", default = "savepoint" } }

function onUpdate(self, dt)
    interact.update(self, 2.2, "Сохранить в точке «" .. self.slot .. "»", function()
        showcase.save(self.slot, "Точка сохранения · " .. (self.slot:find("east") and "восток" or "запад"))
        audio.play("project://Assets/Audio/chime.wav", self.entity.transform.worldPosition, 0.7)
        flow.toast("Сохранено в слот " .. self.slot)
    end)
end

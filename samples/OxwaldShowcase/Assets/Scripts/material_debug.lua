-- Материалы: [V] cycles the renderer debug view so the PBR inputs behind the image become visible.
local flow = require("lib.flow")

local views = {
    { "None", "Итоговое изображение" },
    { "Albedo", "Альбедо (базовый цвет)" },
    { "Normals", "Нормали" },
    { "Roughness", "Шероховатость" },
    { "Metallic", "Металличность" },
}

function onStart(self)
    self.index = 1
end

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    if input.keyPressed("V") then
        self.index = self.index % #views + 1
        showcase.setCVar("r.DebugView", views[self.index][1])
        flow.toast("Режим отладки: " .. views[self.index][2])
    end
end

function onDestroy(self)
    -- Leaving the station must not leave the whole game in a debug view.
    showcase.setCVar("r.DebugView", "None")
end

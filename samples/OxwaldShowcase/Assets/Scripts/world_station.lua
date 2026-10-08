-- Открытый мир: [F] toggles world debug (terrain LOD, streaming chunks); prompts while chunks stream in.
local flow = require("lib.flow")

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    if input.keyPressed("F") then
        self.debug = not self.debug
        showcase.exec("ui.debug")
        flow.toast(self.debug and "Отладка мира: F1 → Debug Draw" or "Отладка выключена")
    end
end

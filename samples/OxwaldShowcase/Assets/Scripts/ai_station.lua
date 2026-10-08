-- ИИ: [F] toggles navmesh / perception debug drawing (gameplay debug draw, also in the F1 overlay).
local flow = require("lib.flow")

function onStart(self)
    -- Tour / photo mode: show the navmesh, paths and perception so the screenshot explains the AI.
    if showcase.mode() ~= "play" then showcase.debugDraw("ai", true) end
end

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    if input.keyPressed("F") then
        self.on = not self.on
        showcase.debugDraw("ai", self.on)
        flow.toast(self.on and "Отладка ИИ: навмеш, пути, восприятие" or "Отладка ИИ выключена")
    end
end

-- Time of day control (Свет и тени / Волюметрика): [T] fast-forwards the day, [N] jumps between day and night.
-- TimeOfDay drives the sun light, sky, ambient, fog and camera exposure; stars and moon appear at night.
local flow = require("lib.flow")

properties = {
    dayNight = { type = "bool", default = false, tooltip = "Run a continuous day/night cycle" },
    cycleSpeed = { type = "float", default = 900.0, tooltip = "Game seconds per real second while [T] is held" },
}

function onStart(self)
    self.tod = self.entity:get("TimeOfDay")
end

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    local h = world.timeOfDay() or 12
    if input.keyDown("T") then
        world.setTimeOfDay((h + dt * self.cycleSpeed / 3600) % 24)
        flow.prompt(string.format("Время суток %02d:%02d", math.floor(h), math.floor((h % 1) * 60)))
        self.showing = true
    elseif self.showing then
        flow.prompt("")
        self.showing = false
    end
    if input.keyPressed("N") then
        world.setTimeOfDay(world.isDay() and 23.5 or 10.0)
        flow.toast(world.isDay() and "Ночь: звёзды и луна" or "День")
    end
    if self.dayNight and input.keyPressed("C") then
        self.tod.paused = not self.tod.paused
        self.tod.timeScale = 300
        flow.toast(self.tod.paused and "Цикл дня остановлен" or "Цикл дня и ночи запущен")
    end
end

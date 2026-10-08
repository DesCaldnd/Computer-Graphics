-- Elevator: kinematic platform moved by a coroutine (await scene.nextFrame / scene.delay); call it from the console.
local interact = require("lib.interact")
local flow = require("lib.flow")
properties = { height = { type = "float", default = 3.9 }, speed = { type = "float", default = 1.4 } }

function onStart(self)
    self.bottom = self.entity.transform.worldPosition
    self.top = self.bottom + vec3(0, self.height, 0)
    self.up = false
    self.busy = false
    self.console = scene.find("ElevatorConsole")
end

local function ride(self)
    self.busy = true
    local from, to = self.up and self.top or self.bottom, self.up and self.bottom or self.top
    audio.play("project://Assets/Audio/chime.wav", from, 0.6)
    await(scene.delay(0.6))
    local d = from:distance(to)
    local t = 0
    while t < d / self.speed do
        t = t + 1 / 60
        self.entity.transform.worldPosition = from:lerp(to, math.smoothstep(0, 1, t * self.speed / d))
        await(scene.nextFrame())
    end
    self.entity.transform.worldPosition = to
    self.up = not self.up
    flow.toast(self.up and "Лифт наверху" or "Лифт внизу")
    self.busy = false
end

function onUpdate(self, dt)
    if self.busy then return end
    -- The console next to the shaft is the interactable.
    self.proxy = self.proxy or { entity = self.console or self.entity }
    interact.update(self.proxy, 2.2, self.up and "Опустить лифт" or "Поднять лифт", function()
        spawn(function() ride(self) end)
    end)
end

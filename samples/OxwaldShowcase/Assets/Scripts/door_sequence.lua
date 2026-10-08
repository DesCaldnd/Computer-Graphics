-- Vault door sequence written as one coroutine with await/wait: alarm light → unlock → slide up → hold → close.
local interact = require("lib.interact")
local flow = require("lib.flow")

local function tween(e, from, to, seconds)
    local t = 0
    while t < seconds do
        t = t + 1 / 60
        local k = math.smoothstep(0, 1, math.min(1, t / seconds))
        e.transform.worldPosition = from:lerp(to, k)
        await(scene.nextFrame())
    end
    e.transform.worldPosition = to
end

function onStart(self)
    self.door = scene.find("VaultDoor")
    self.alarm = scene.find("AlarmLight"):get("Light")
    self.closed = self.door.transform.worldPosition
    self.busy = false
end

local function sequence(self)
    self.busy = true
    flow.toast("Корутина: тревога → разблокировка → дверь")
    for i = 1, 6 do
        self.alarm.intensity = (i % 2 == 1) and 4000 or 0
        await(scene.delay(0.25))
    end
    self.alarm.intensity = 0
    audio.play("project://Assets/Audio/door.wav", self.closed, 0.9)
    tween(self.door, self.closed, self.closed + vec3(0, 3.1, 0), 2.0)
    await(scene.delay(4.0))
    audio.play("project://Assets/Audio/door.wav", self.closed, 0.9)
    tween(self.door, self.closed + vec3(0, 3.1, 0), self.closed, 1.6)
    self.busy = false
end

function onUpdate(self, dt)
    if self.busy then return end
    interact.update(self, 2.2, "Открыть хранилище", function() spawn(function() sequence(self) end) end)
end

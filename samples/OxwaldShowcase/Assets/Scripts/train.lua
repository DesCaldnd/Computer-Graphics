-- Train: constant speed along the closed track (SplineFollower). Slows down at the "Station" marker, waits,
-- whistles and departs again — written as a coroutine started from onSplineEvent.
local flow = require("lib.flow")

function onStart(self)
    self.cars = {}
    for _, e in ipairs(scene.findAll("SplineFollower")) do
        if e.name == "Locomotive" or e.name == "Wagon" then self.cars[#self.cars + 1] = e:get("SplineFollower") end
    end
    self.cruise = 6.0
end

local function setSpeed(self, v)
    for _, f in ipairs(self.cars) do f.speed = v end
end

function onSplineEvent(self, name, distance)
    if name ~= "Station" or self.stopping then return end
    self.stopping = true
    spawn(function()
        for i = 1, 30 do setSpeed(self, self.cruise * (1 - i / 30)); wait(0.05) end
        setSpeed(self, 0)
        audio.play("project://Assets/Audio/chime.wav", self.entity.transform.worldPosition, 0.8)
        wait(3.0)
        for i = 1, 40 do setSpeed(self, self.cruise * i / 40); wait(0.05) end
        self.stopping = false
    end)
end

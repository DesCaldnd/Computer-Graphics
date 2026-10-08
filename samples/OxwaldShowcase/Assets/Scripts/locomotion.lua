-- Locomotion: the spline follower speed drives the animator's "speed" parameter (idle → walk → run).
-- [1] idle, [2] walk, [3] run; the walker smoothly accelerates to the selected speed.
local flow = require("lib.flow")
properties = { fixedSpeed = { type = "float", default = 0, tooltip = "> 0: keep this speed (keys ignored)" } }

function onStart(self)
    self.follower = self.entity:get("SplineFollower")
    self.speed = self.follower and self.follower.speed or 0
    self.target = self.fixedSpeed > 0 and self.fixedSpeed or self.speed
end

function onUpdate(self, dt)
    if not self.follower then return end
    if self.fixedSpeed <= 0 and showcase.mode() == "play" and not flow.blocking() then
        if input.keyPressed("Num1") then self.target = 0; flow.toast("Манекен: стоит (Idle)") end
        if input.keyPressed("Num2") then self.target = 1.4; flow.toast("Манекен: шаг (Walk)") end
        if input.keyPressed("Num3") then self.target = 4.5; flow.toast("Манекен: бег (Run)") end
    end
    self.speed = math.lerp(self.speed, self.target, math.min(1, dt * 1.5))
    self.follower.speed = self.speed
    self.follower.playing = self.speed > 0.05
    if self.entity.animator then self.entity.animator:setFloat("speed", self.speed) end
end

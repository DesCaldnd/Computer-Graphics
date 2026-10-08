-- Third-person orbit camera: mouse / right stick look, wheel zoom, keeps the player in view by casting a ray from
-- the pivot (walls push the camera closer). Inactive in tour/photo mode (the tour drives its own camera).
local flow = require("lib.flow")

properties = {
    distance = { type = "float", default = 4.5, min = 1.5, max = 14 },
    height = { type = "float", default = 1.55, min = 0, max = 4 },
    sensitivity = { type = "float", default = 0.0022, min = 0.0002, max = 0.02 },
    stickSpeed = { type = "float", default = 2.6, min = 0.1, max = 10 },
}

function onStart(self)
    self.target = scene.find("Player")
    local f = self.entity.transform.forward
    self.yaw = math.atan(-f.x, -f.z)
    self.pitch = -0.22
    self.dist = self.distance
end

function onUpdate(self, dt)
    if not self.target or showcase.mode() ~= "play" then return end
    if not flow.blocking() and not flow.inMainMenu then
        local md = input.mouseDelta()
        local pad = input.action("Look")
        self.yaw = self.yaw - md.x * self.sensitivity - pad.x * self.stickSpeed * dt
        self.pitch = math.clamp(self.pitch - md.y * self.sensitivity + pad.y * self.stickSpeed * 0.8 * dt, -1.25, 0.7)
        local wheel = input.mouseWheel()
        if wheel ~= 0 then self.distance = math.clamp(self.distance - wheel * 0.5, 1.5, 14) end
    end
    local pivot = self.target.transform.worldPosition + vec3(0, self.height, 0)
    local rot = quat.angleAxis(self.yaw, vec3(0, 1, 0)) * quat.angleAxis(self.pitch, vec3(1, 0, 0))
    local back = rot * vec3(0, 0, 1)
    local want = self.distance
    local hit = physics.raycast(pivot, back, self.distance + 0.3, self.target)
    if hit then want = math.max(0.6, hit.distance - 0.3) end
    -- Pull in fast, ease out slowly.
    if want < self.dist then self.dist = want else self.dist = math.lerp(self.dist, want, math.min(1, dt * 4)) end
    local t = self.entity.transform
    t.worldPosition = pivot + back * self.dist
    t.worldRotation = rot
end

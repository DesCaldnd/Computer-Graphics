-- Main menu backdrop: slow orbit around the stage.
properties = {
    radius = { type = "float", default = 14.0 },
    height = { type = "float", default = 3.2 },
    speed = { type = "float", default = 0.08 },
}

function onStart(self)
    local p = self.entity.transform.worldPosition
    self.angle = math.atan(p.x, p.z)
end

function onUpdate(self, dt)
    if showcase.mode() ~= "play" then return end
    self.angle = self.angle + dt * self.speed
    local pos = vec3(math.sin(self.angle) * self.radius, self.height, math.cos(self.angle) * self.radius)
    local t = self.entity.transform
    t.worldPosition = pos
    t:lookAt(vec3(0, 1.2, 0))
end

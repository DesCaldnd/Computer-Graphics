-- Server-side avatar of the bot client: runs a figure eight. The bot client sees it (and the player and the
-- crates) through replication; Showcase's NetDemo mirrors the client's interpolated view as ghosts.
properties = { speed = { type = "float", default = 0.5 }, size = { type = "float", default = 6.0 } }

function onStart(self) self.t = 0; self.center = self.entity.transform.worldPosition end

function onUpdate(self, dt)
    self.t = self.t + dt * self.speed
    local p = self.center + vec3(math.sin(self.t) * self.size, 0, math.sin(self.t * 2) * self.size * 0.45)
    local prev = self.entity.transform.worldPosition
    self.entity.transform.worldPosition = p
    local d = p - prev
    if d:length() > 1e-4 then self.entity.transform.worldRotation = quat.lookRotation(vec3(d.x, 0, d.z):normalize()) end
end

-- Places the entity on the procedural terrain once its heightfield exists (world station props).
properties = { offset = { type = "float", default = 0.0 } }

function onUpdate(self, dt)
    if self.done then return end
    local p = self.entity.transform.worldPosition
    local h = world.terrainHeight(p.x, p.z)
    if h then
        self.entity.transform.worldPosition = vec3(p.x, h + self.offset, p.z)
        self.done = true
    end
end

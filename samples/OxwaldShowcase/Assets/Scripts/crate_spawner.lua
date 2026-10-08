-- Вода: [B] drops a floating crate (prefab "FloatingCrate": RigidBody + Buoyancy) into the water.
local flow = require("lib.flow")

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    if input.keyPressed("B") then
        local p = self.entity.transform.worldPosition + vec3(math.random() * 6 - 3, 0, math.random() * 6 - 3)
        local crate = scene.spawn("FloatingCrate", p, quat.fromEuler(vec3(math.random(), math.random(), math.random())))
        if crate then flow.toast("Ящик сброшен — плавучесть считает погружённый объём") end
    end
end

-- Физика: [R] rebuilds the box towers (records the start poses, teleports the blocks back).
local flow = require("lib.flow")

function onStart(self)
    self.blocks = {}
    for _, e in ipairs(scene.findAll("RigidBody")) do
        if e:hasTag("TowerBlock") then
            self.blocks[#self.blocks + 1] = { e = e, p = e.transform.worldPosition, r = e.transform.worldRotation }
        end
    end
end

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    if input.keyPressed("R") then
        for _, b in ipairs(self.blocks) do
            b.e.body:teleport(b.p, b.r)
            b.e.body.linearVelocity = vec3(0, 0, 0)
            b.e.body.angularVelocity = vec3(0, 0, 0)
        end
        flow.toast("Башни собраны заново")
    end
end

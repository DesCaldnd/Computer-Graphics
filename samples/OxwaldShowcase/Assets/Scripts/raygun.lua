-- Ray gun: [LMB]/[E] casts physics.raycast from the camera and pushes the body that was hit.
local flow = require("lib.flow")
properties = {
    range = { type = "float", default = 60 },
    impulse = { type = "float", default = 18, tooltip = "m/s velocity change at the hit point" },
}

function onStart(self) self.cam = scene.find("PlayerCamera") end

function onUpdate(self, dt)
    if showcase.mode() ~= "play" or flow.blocking() or not self.cam then return end
    if input.triggered("Fire") or input.triggered("Interact") then
        local t = self.cam.transform
        local origin = t.worldPosition
        local hit = physics.raycast(origin, t.forward, self.range, scene.find("Player"))
        audio.play("project://Assets/Audio/raygun.wav", origin, 0.7)
        if hit and hit.entity and hit.entity:has("RigidBody") then
            local rb = hit.entity:get("RigidBody")
            if rb.motionType == "Dynamic" then
                local m = rb.mass > 0 and rb.mass or 20
                hit.entity.body:addImpulse(t.forward * (self.impulse * m), hit.point)
                flow.toast(string.format("Попадание: %s (%.1f м)", hit.entity.name, hit.distance), 1.2)
            end
        end
    end
end

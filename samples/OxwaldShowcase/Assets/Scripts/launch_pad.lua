-- Trigger pad: every dynamic body (or the player) entering it is launched upwards.
properties = { impulse = { type = "float", default = 9.0, tooltip = "Velocity change (m/s)" } }

function onTriggerEnter(self, other)
    if other.name == "Player" then
        local cc = other:get("CharacterController")
        cc.jump = true
        return
    end
    if other:has("RigidBody") then
        local rb = other:get("RigidBody")
        local m = rb.mass > 0 and rb.mass or 20
        other.body:addImpulse(vec3(0, self.impulse * m, 0))
        audio.play("project://Assets/Audio/beep.wav", self.entity.transform.worldPosition, 0.5)
    end
end

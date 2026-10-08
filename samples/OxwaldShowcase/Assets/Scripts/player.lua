-- Third-person character: CharacterController driven by the "Move"/"Sprint"/"Jump" input actions relative to the
-- camera, turns towards the walking direction, footsteps. [E] (Interact) is handled by the objects themselves.
local flow = require("lib.flow")

properties = {
    walkSpeed = { type = "float", default = 4.0, min = 0, max = 20 },
    sprintSpeed = { type = "float", default = 7.5, min = 0, max = 30 },
    turnSpeed = { type = "float", default = 12.0, min = 1, max = 40 },
    snapToTerrain = { type = "bool", default = false, tooltip = "Drop onto the terrain at start (world station)" },
}

local function controlsEnabled()
    return showcase.mode() == "play" and not flow.blocking() and not flow.inMainMenu
end

function onStart(self)
    self.cc = self.entity:get("CharacterController")
    self.cam = scene.find("PlayerCamera")
    self.stepTimer = 0
    self.snapTries = 0
end

local function trySnap(self)
    local p = self.entity.transform.worldPosition
    local h = world.terrainHeight(p.x, p.z)
    if h then
        self.entity.transform.worldPosition = vec3(p.x, h + 0.2, p.z)
        self.snapToTerrain = false
    end
end

function onUpdate(self, dt)
    if self.snapToTerrain then trySnap(self) end
    local move = controlsEnabled() and input.action("Move") or vec2(0, 0)
    local fwd = self.cam and self.cam.transform.forward or vec3(0, 0, -1)
    fwd = vec3(fwd.x, 0, fwd.z)
    if fwd:length() < 1e-3 then fwd = vec3(0, 0, -1) end
    fwd = fwd:normalize()
    local right = vec3(-fwd.z, 0, fwd.x)
    local dir = right * move.x + fwd * move.y
    if dir:length() > 1 then dir = dir:normalize() end
    local sprint = controlsEnabled() and input.action("Sprint")
    local speed = sprint and self.sprintSpeed or self.walkSpeed
    self.cc.desiredVelocity = dir * speed
    local grounded = self.cc.groundState == "OnGround"
    if controlsEnabled() and input.triggered("Jump") and grounded then self.cc.jump = true end
    if dir:length() > 0.1 then
        local t = self.entity.transform
        t.rotation = t.rotation:slerp(quat.lookRotation(dir), math.min(1, dt * self.turnSpeed))
        if grounded then
            self.stepTimer = self.stepTimer - dt * (sprint and 1.6 or 1.0)
            if self.stepTimer <= 0 then
                self.stepTimer = 0.42
                audio.play("project://Assets/Audio/footstep.wav", t.worldPosition, 0.35)
            end
        end
    end
end

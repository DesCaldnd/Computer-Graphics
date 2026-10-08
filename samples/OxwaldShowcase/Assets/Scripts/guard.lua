-- Guard (behaviour tree AI/guard.oxbt): patrol waypoints → chase while the player is visible → search the last
-- known position → give up. The tree calls the functions below through ScriptAction nodes; perception events
-- update the blackboard. The eye light shows the state (green patrol, red chase, yellow search).
properties = {
    route = { type = "string", default = "", tooltip = "x,z;x,z;... patrol waypoints" },
}

local function setEye(self, color)
    if self.eye then self.eye.color = color end
end

function onStart(self)
    self.points = {}
    for x, z in self.route:gmatch("(-?[%d%.]+),(-?[%d%.]+)") do
        self.points[#self.points + 1] = vec3(tonumber(x), 0, tonumber(z))
    end
    self.index = 0
    local eye = self.entity:findChild("EyeLight")
    self.eye = eye and eye:get("Light")
    self.perception = self.entity:get("Perception")
    self.lastSeen = nil
end

function nextWaypoint(self)
    if #self.points == 0 then return false end
    self.index = self.index % #self.points + 1
    ai.blackboardSet(self.entity, "waypoint", self.points[self.index])
    setEye(self, vec3(0.2, 1.0, 0.3))
    return true
end

function onChase(self)
    setEye(self, vec3(1.0, 0.1, 0.05))
    return true
end

function onSearch(self)
    setEye(self, vec3(1.0, 0.8, 0.1))
    return true
end

function lookAround(self)
    self.entity.transform:rotate(vec3(0, 1, 0), math.pi * 0.5)
    return true
end

function giveUp(self)
    ai.blackboardSet(self.entity, "searching", false)
    return true
end

function onUpdate(self, dt)
    local p = self.perception
    if p and p.targetVisible then
        local target = scene.find("Player")
        if target then self.lastSeen = target.transform.worldPosition end
    end
    -- Animation speed from the agent's velocity.
    if self.entity.animator and self.entity.agent then
        self.entity.animator:setFloat("speed", self.entity.agent.velocity:length())
    end
end

function onTargetSensed(self, source, sense)
    audio.play("project://Assets/Audio/beep.wav", self.entity.transform.worldPosition, 0.6)
end

function onTargetLost(self, source, sense)
    if self.lastSeen then
        ai.blackboardSet(self.entity, "lastKnown", self.lastSeen)
        ai.blackboardSet(self.entity, "searching", true)
    end
end

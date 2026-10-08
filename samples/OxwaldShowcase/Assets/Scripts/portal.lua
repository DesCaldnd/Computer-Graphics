-- Portal trigger: walking in loads the target level (asynchronous level change, autosave of the level left).
local flow = require("lib.flow")

properties = {
    target = { type = "string", default = "project://Assets/Scenes/Hub.oxscene" },
    label = { type = "string", default = "" },
}

local function title(self)
    if self.label == "hub" then return "Хаб" end
    for _, s in ipairs(showcase.stations()) do
        if s.id == self.label then return s.title end
    end
    return self.label
end

function onStart(self)
    self.cooldown = 1.5 -- do not bounce back when spawning next to a portal
    self.player = scene.find("Player")
end

function onUpdate(self, dt)
    self.cooldown = math.max(0, self.cooldown - dt)
    if self.player and showcase.mode() == "play" then
        local d = self.entity.transform.worldPosition:distance(self.player.transform.worldPosition)
        if d < 5 then
            flow.prompt("Портал → " .. title(self))
            self.prompting = true
        elseif self.prompting then
            flow.prompt("")
            self.prompting = false
        end
    end
end

function onTriggerEnter(self, other)
    if self.cooldown > 0 or showcase.mode() ~= "play" or showcase.loading() then return end
    if other.name ~= "Player" then return end
    flow.prompt("")
    flow.toast("Переход: " .. title(self), 2)
    audio.play("project://Assets/Audio/chime.wav", nil, 0.6)
    showcase.loadLevel(self.target)
end

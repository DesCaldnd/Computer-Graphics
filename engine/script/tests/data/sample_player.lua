-- Sample gameplay script (also used in docs/dev/modules/script.md and the script tests).
local util = require("util.mathx")

properties = {
    speed     = { type = "float", default = 5, min = 0, max = 20, tooltip = "Units per second", order = 0 },
    maxHealth = { type = "int", default = 100, min = 1, max = 1000, order = 1 },
    tint      = { type = "color", default = { 1, 0.5, 0, 1 } },
    spawnAt   = { type = "vec3", default = vec3(0, 1, 0) },
    godMode   = false, -- shorthand: type inferred (bool)
}

function onCreate(self)
    self.health = self.maxHealth
    self.position = self.spawnAt:clone()
    self.ticks = 0
    events.subscribe("player.damage", function(name, payload)
        if not self.godMode then
            self.health = math.max(0, self.health - payload.amount)
        end
    end)
end

function onStart(self)
    spawn(function()
        wait(1.0)
        self.warmedUp = true
        log.info("player warmed up at", time())
    end)
end

function onUpdate(self, dt)
    self.ticks = self.ticks + 1
    self.position = self.position + vec3.forward * (self.speed * dt)
    self.distance = util.round2(self.position:distance(self.spawnAt))
end

function onEvent(self, name, payload)
    if name == "heal" then
        self.health = math.min(self.maxHealth, self.health + payload)
    end
end

function onDestroy(self)
    events.publish("player.died", { ticks = self.ticks })
end

function on_reload(self)
    log.info("reloaded, health kept:", self.health)
end

-- Глава 15: турель. Свойства видны в инспекторе редактора, состояние живёт в self.
local aim = require("util.aim")

properties = {
    range    = { type = "float", default = 15, min = 1, max = 50, tooltip = "Дальность, м", order = 0 },
    fireRate = { type = "float", default = 2, min = 0.1, max = 10, tooltip = "Выстрелов в секунду", order = 1 },
    ammo     = { type = "int", default = 5, min = 0, max = 100, order = 2 },
    tint     = { type = "color", default = { 1, 0.2, 0.2, 1 }, order = 3 },
    muzzle   = { type = "vec3", default = vec3(0, 1.5, 0), order = 4 },
    friendly = false, -- короткая форма: bool со значением по умолчанию
}

function onCreate(self)
    self.shots = 0
    self.state = "off"
    -- Подписка принадлежит экземпляру и снимается при destroy().
    events.subscribe("enemy.spotted", function(name, enemy)
        if enemy.distance <= self.range then self.target = enemy.position end
    end)
end

function onStart(self)
    spawn(function()          -- корутина на времени скрипта
        self.state = "warming"
        wait(1.0)
        self.state = "ready"
        log.info("turret ready, ammo:", self.ammo)
    end)
end

function onUpdate(self, dt)
    if self.state ~= "ready" or self.target == nil or self.friendly then return end
    self.yaw = aim.yawTo(self.muzzle, self.target)
    self.cooldown = (self.cooldown or 0) - dt
    if self.cooldown <= 0 and self.ammo > 0 then
        self.ammo = self.ammo - 1
        self.shots = self.shots + 1
        self.cooldown = 1 / self.fireRate
        events.publish("turret.fired", { shots = self.shots, yaw = self.yaw })
    end
end

function onEvent(self, name, payload)
    if name == "reload" then self.ammo = self.ammo + payload end
end

function onDestroy(self)
    events.publish("turret.destroyed", { shots = self.shots })
end

-- Глава 32: бонус. Вращается; когда через него пролетает игрок, начисляет очки и исчезает.
properties = {
    spinSpeed = { type = "float", default = 90, min = 0, max = 720, tooltip = "Градусов в секунду" },
    score     = { type = "int", default = 10, min = 0, max = 1000 },
}

function onStart(self)
    local e = self.entity
    e.transform.position = vec3(0, 1, 0)              -- локальная позиция
    local col = e:get("Collider")                     -- прокси отражённого компонента
    col.type = "Sphere"                               -- enum по имени
    col.radius = 0.5
    e:add("Trigger", { requiredTag = "player" })      -- коллайдер становится сенсором
    self.spun = 0
end

function onUpdate(self, dt)
    local angle = math.rad(self.spinSpeed) * dt
    self.entity.transform:rotate(vec3(0, 1, 0), angle)
    self.spun = self.spun + angle
end

function onTriggerEnter(self, other)
    other:sendEvent("pickup", { score = self.score }) -- -> onEvent(self, name, payload) скрипта игрока
    scene.destroy(self.entity)
end

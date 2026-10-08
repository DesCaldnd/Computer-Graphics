-- Глава 32: игрок копит очки от бонусов и читает поля своих компонентов.
function onCreate(self)
    self.score = 0
    local rb = self.entity:get("RigidBody")
    self.massAtStart = rb.mass
    rb.mass = 80                                      -- запись поля: тело пересоздастся на ближайшем шаге
    self.motion = rb.motionType                       -- "Dynamic"
end

function onEvent(self, name, payload)
    if name == "pickup" then
        self.score = self.score + payload.score
        self.lastPickupY = self.entity.transform.worldPosition.y
    end
end

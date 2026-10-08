-- Глава 15: управление через таблицу `input` (привязки runtime, bindInputLuaApi).
properties = {
    speed = { type = "float", default = 4, min = 0, max = 20 },
}

function onCreate(self)
    self.position = vec3(0, 0, 0)
    self.jumps = 0
end

function onUpdate(self, dt)
    local move = input.action("Move")                 -- Axis2D -> vec2
    self.position = self.position + vec3(move.x, 0, -move.y) * (self.speed * dt)
    if input.triggered("Jump") then self.jumps = self.jumps + 1 end
    self.sprinting = input.keyDown("LeftShift")       -- «сырые» клавиши тоже доступны
end

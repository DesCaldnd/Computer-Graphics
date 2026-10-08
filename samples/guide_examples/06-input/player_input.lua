-- Глава 06: чтение ввода из Lua через глобальную таблицу `input` (только чтение).
local speed = 5.0

-- Возвращает смещение игрока за кадр и флаги действий.
function readPlayerInput(dt)
    local move = input.action("Move")          -- Axis2D -> vec2 (move.x, move.y)
    local sprint = input.keyDown("LeftShift")  -- сырое состояние клавиши
    local k = sprint and 2.0 or 1.0
    return {
        dx = move.x * speed * k * dt,
        dz = move.y * speed * k * dt,
        jump = input.triggered("Jump"),        -- фронт действия в этом кадре
        look = input.mouseDelta(),             -- vec2, пиксели за кадр
    }
end

function lockCursorForGameplay()
    input.setCursorMode("Locked")              -- "Normal" | "Hidden" | "Locked"
end

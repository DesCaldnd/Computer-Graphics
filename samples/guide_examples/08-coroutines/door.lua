-- Глава 08: Lua-дверь, которая ждёт C++ Future через await (docs/guide/08-coroutines.md).
properties = {
    openAngle = { type = "float", default = 90, min = 0, max = 180, tooltip = "Угол открытия, градусы" },
}

function onCreate(self)
    self.state = "closed"
end

function onStart(self)
    -- Ошибка C++ Future превращается в Lua-ошибку: ловим pcall.
    spawn(function()
        local ok, err = pcall(function() return await(assets.load("sfx/missing.ogg")) end)
        if not ok then self.loadError = err end
    end)
end

-- Вызывается из C++: co_await bridge.invoke(*instance, "open", 45.0) -> open(self, 45.0)
function open(self, degreesPerSecond)
    self.state = "opening"
    self.clip = await(assets.load("anims/door_open.anim")) -- корутина «паркуется» без опроса
    wait(self.openAngle / degreesPerSecond)                 -- время скрипта
    self.state = "open"
    return self.openAngle
end

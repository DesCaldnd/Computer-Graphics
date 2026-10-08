-- Глава 29: игровой UI из Lua. Скрипт создаёт модель данных, загружает документ ui/quest.rml,
-- подписывается на кнопки и рисует отладочное окно ImGui.
local quest

function onCreate(self)
    self.accepted = 0
    self.closed = false
    -- Модель создаётся ДО загрузки документа, который на неё ссылается (data-model="quest").
    quest = ui.createModel("quest", { title = "Волки у мельницы", reward = 50 }, {
        -- data-event-click="accept(reward)": аргументы приходят таблицей args[1], args[2], ...
        accept = function(args) self.accepted = self.accepted + args[1] end,
    })
    ui.load("quest.rml", true)
    ui.on("quest.rml", "close", "click", function(ev)
        self.closed = (ev.type == "click" and ev.target == "close")
        ui.hide("quest.rml")
    end)
end

function onUpdate(self, dt)
    -- Немедленный режим: окно рисуется, пока скрипт вызывает debug.window каждый кадр.
    debug.window("Quest debug", function()
        debug.text("reward", quest:get("reward"))
        local reward, changed = debug.sliderFloat("reward", quest:get("reward"), 0, 500)
        if changed then quest:set("reward", math.floor(reward)) end
        if debug.button("Double reward") then quest:set("reward", quest:get("reward") * 2) end
    end)
end

-- Вызывается из теста: меняет поле модели, документ обновится на следующем кадре.
function setReward(self, value)
    quest:set("reward", value)
end

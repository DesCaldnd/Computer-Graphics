-- Dialogue: a coroutine shows lines and awaits the player's choice (a Future completed by a UI click).
local interact = require("lib.interact")
local flow = require("lib.flow")
properties = { speaker = { type = "string", default = "NPC" } }

local doc = flow.docs.dialogue

local script = {
    { line = "Добро пожаловать на станцию! Поезд ходит по сплайну Catmull-Rom с постоянной скоростью.",
      choices = { "Как он едет так плавно?", "А двери?", "Пока!" } },
    [1] = "Параметризация по длине дуги: SplineFollower двигается на speed метров в секунду, где бы ни стояли точки.",
    [2] = "Двери и лифт — корутины на Lua: await(scene.delay(…)) вместо таймеров и флагов состояния.",
    [3] = "Удачной экскурсии!",
}

-- Waits for a click on one of the generated choice buttons (await on a polled condition).
local function awaitChoice(self)
    self.choice = nil
    while not self.choice do await(scene.nextFrame()) end
    return self.choice
end

local function run(self)
    self.active = true
    flow.closeAll()
    input.setCursorMode("Normal")
    ui.setText(doc, "speaker", self.speaker)
    local first = script[1]
    ui.setText(doc, "line", first.line)
    local buttons = {}
    for i, c in ipairs(first.choices) do buttons[#buttons + 1] = string.format('<button id="choice-%d">%d. %s</button>', i, i, c) end
    ui.setText(doc, "choices", table.concat(buttons))
    ui.show(doc)
    local pick = awaitChoice(self)
    ui.setText(doc, "line", script[pick] or "…")
    ui.setText(doc, "choices", '<button id="choice-9">Понятно</button>')
    awaitChoice(self)
    ui.hide(doc)
    if showcase.mode() == "play" then input.setCursorMode("Locked") end
    self.active = false
end

function onStart(self)
    self.listener = ui.on(doc, "choices", "click", function(ev)
        local n = tonumber((ev.target or ""):match("^choice%-(%d)$"))
        if n then self.choice = n end
    end)
end

function onUpdate(self, dt)
    if self.active then
        for i = 1, 3 do if input.keyPressed("Num" .. i) then self.choice = i end end
        return
    end
    interact.update(self, 2.5, "Поговорить", function() spawn(function() run(self) end) end)
end

function onDestroy(self)
    if self.listener then ui.off(self.listener) end
    ui.hide(doc)
end

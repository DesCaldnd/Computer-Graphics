-- Terminal: opens the save slot browser (load / delete / new manual save).
local interact = require("lib.interact")
local flow = require("lib.flow")

function onUpdate(self, dt)
    interact.update(self, 2.4, "Открыть браузер сохранений", function()
        flow.refreshSaves()
        flow.push(flow.docs.saves)
    end)
end

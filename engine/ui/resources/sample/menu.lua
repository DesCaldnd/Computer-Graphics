-- Sample game UI flow (main menu -> settings, HUD) using the Lua `ui` API. Copy the .rml/.rcss files of this folder
-- into <project>/UI/ and attach this script to an entity (or run it from the startup script).
local hud

local function showMenu()
    ui.hide("hud.rml")
    ui.show("main_menu.rml")
end

function onCreate(self)
    -- Data models must exist before the documents that use them are loaded.
    hud = ui.createModel("hud", { health = 100, maxHealth = 100, ammo = 30, reserve = 90,
                                  objective = "Найдите выход из лаборатории" })
    ui.load("main_menu.rml", true)
    ui.load("settings.rml")
    ui.load("hud.rml")

    ui.on("main_menu.rml", "play", "click", function()
        ui.hide("main_menu.rml")
        ui.show("hud.rml")
        input.setCursorMode("Locked")
    end)
    ui.on("main_menu.rml", "settings", "click", function()
        ui.hide("main_menu.rml")
        ui.show("settings.rml", true)
    end)
    ui.on("main_menu.rml", "quit", "click", function() events.publish("game.quit", {}) end)
    ui.on("settings.rml", "back", "click", function()
        ui.hide("settings.rml")
        ui.show("main_menu.rml")
    end)
    ui.on("hud.rml", "pause", "click", function()
        input.setCursorMode("Normal")
        showMenu()
    end)
end

function onUpdate(self, dt)
    if ui.isVisible("hud.rml") and input.keyPressed("Escape") then
        input.setCursorMode("Normal")
        showMenu()
    end
    -- Quick debug window (shown while the game runs; F1 opens the engine's debug overlay).
    debug.window("HUD debug", function()
        local h = hud:get("health")
        local newH, changed = debug.sliderFloat("health", h, 0, 100)
        if changed then hud:set("health", math.floor(newH)) end
        if debug.button("-10 ammo") then hud:set("ammo", math.max(0, hud:get("ammo") - 10)) end
    end)
end

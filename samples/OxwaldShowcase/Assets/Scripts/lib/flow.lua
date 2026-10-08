-- Shared game flow of the showcase: HUD model, main/pause menus, settings, save browser, toasts.
-- A module: `require("lib.flow")` returns the same table in every scene (the VM caches it), so documents, data
-- models and UI listeners are created once and survive level changes.
local flow = {}

local UI = "project://Assets/UI/"
flow.UI = UI
flow.docs = {
    hud = UI .. "hud.rml",
    menu = UI .. "main_menu.rml",
    pause = UI .. "pause.rml",
    settings = UI .. "settings.rml",
    saves = UI .. "saves.rml",
    dialogue = UI .. "dialogue.rml",
    side = UI .. "side_panel.rml",
}

local initialized = false
local hud, menuModel
local stack = {}          -- open menus (top = last)
local toastUntil = 0
local deleteMode = false
local onStation = nil     -- function(): info about the current scene (set by game.lua)

-- ---------------------------------------------------------------------------------------------------- helpers
local function now() return time() end

function flow.hud() return hud end

function flow.toast(text, seconds)
    if not hud then return end
    hud:set("toast", text)
    toastUntil = now() + (seconds or 2.5)
end

function flow.prompt(text) if hud then hud:set("prompt", text or "") end end

function flow.blocking() return #stack > 0 end

function flow.isPlayMode() return showcase.mode() == "play" end

local function setCursor()
    if not flow.isPlayMode() then return end
    if #stack > 0 or flow.inMainMenu then input.setCursorMode("Normal") else input.setCursorMode("Locked") end
end

local function showOnly(doc)
    for _, d in pairs({flow.docs.menu, flow.docs.pause, flow.docs.settings, flow.docs.saves}) do
        if d ~= doc then ui.hide(d) end
    end
    if doc then ui.show(doc, true) end
end

function flow.push(doc)
    stack[#stack + 1] = doc
    showOnly(doc)
    if not flow.inMainMenu then showcase.setPaused(true) end
    setCursor()
end

function flow.pop()
    stack[#stack] = nil
    local top = stack[#stack]
    showOnly(top)
    if #stack == 0 and not flow.inMainMenu then showcase.setPaused(false) end
    setCursor()
end

function flow.closeAll()
    stack = {}
    showOnly(nil)
    showcase.setPaused(false)
    setCursor()
end

local function levelName(uri)
    if not uri or uri == "" then return "—" end
    for _, s in ipairs(showcase.stations()) do
        if s.scene == uri then return s.title end
    end
    if uri:find("Hub") then return "Хаб" end
    if uri:find("MainMenu") then return "Главное меню" end
    return uri
end
flow.levelName = levelName

-- Newest save of a playable level (autosaves of the main menu are skipped).
function flow.newestSave()
    for _, s in ipairs(showcase.saves()) do
        if not s.corrupted and not s.level:find("MainMenu") then return s end
    end
    return nil
end

-- ---------------------------------------------------------------------------------------------------- saves UI
local function refreshSaves()
    local rml = {}
    local saves = showcase.saves()
    if #saves == 0 then rml[#rml + 1] = '<div class="status">Сохранений пока нет.</div>' end
    for i, s in ipairs(saves) do
        local kind = ({ auto = "автосохранение", quick = "быстрое", manual = "ручное" })[s.kind] or s.kind
        rml[#rml + 1] = string.format(
            '<div class="slot" id="slot-%d"><div class="sn" id="slotname-%d">%s%s</div>' ..
            '<div class="sm" id="slotmeta-%d">%s · %s · время в игре %s · %s · %d сущн.</div></div>',
            i, i, deleteMode and "✕ " or "", s.name, i, levelName(s.level), s.date, s.playTime, kind, s.entities)
    end
    ui.setText(flow.docs.saves, "slots", table.concat(rml))
    ui.setText(flow.docs.saves, "deletemode", deleteMode and "Готово" or "Режим удаления")
    flow.saveList = saves
end
flow.refreshSaves = refreshSaves

local function slotFromTarget(target)
    local n = target and tonumber(target:match("^slot%-(%d+)$") or target:match("^slotname%-(%d+)$") or target:match("^slotmeta%-(%d+)$"))
    return n and flow.saveList and flow.saveList[n] or nil
end

-- ---------------------------------------------------------------------------------------------------- init
function flow.init(stationFn)
    onStation = stationFn
    if initialized then return end
    initialized = true
    hud = ui.createModel("hud", {
        badgeVisible = true, stationNum = "", stationTitle = "",
        infoVisible = false, infoTitle = "", infoDesc = "", infoHints = "", infoGuide = "",
        prompt = "", toast = "", saving = "", fps = "", crosshair = false,
        captionVisible = false, captionTitle = "", captionText = "", captionStep = "",
    })
    menuModel = ui.createModel("menu", { canContinue = false, version = "OxwaldShowcase 1.0 · OxwaldEngine" })
    ui.load(flow.docs.hud, true)
    ui.load(flow.docs.menu)
    ui.load(flow.docs.pause)
    ui.load(flow.docs.settings)
    ui.load(flow.docs.saves)
    ui.load(flow.docs.dialogue)
    ui.load(flow.docs.side)
    require("lib.settings_menu").init(flow)

    -- Main menu.
    ui.on(flow.docs.menu, "continue", "click", function()
        local s = flow.newestSave()
        if s then flow.closeAll(); flow.inMainMenu = false; showcase.load(s.slot) end
    end)
    ui.on(flow.docs.menu, "newtour", "click", function()
        flow.closeAll(); flow.inMainMenu = false; showcase.loadLevel("project://Assets/Scenes/Hub.oxscene")
    end)
    ui.on(flow.docs.menu, "guided", "click", function()
        flow.closeAll(); flow.inMainMenu = false
        showcase.startTour() -- C++ coroutines fly through every station (Source/tour.cpp)
    end)
    ui.on(flow.docs.menu, "load", "click", function() refreshSaves(); flow.push(flow.docs.saves) end)
    ui.on(flow.docs.menu, "settings", "click", function() flow.push(flow.docs.settings) end)
    ui.on(flow.docs.menu, "quit", "click", function() showcase.quit() end)

    -- Pause menu.
    ui.on(flow.docs.pause, "resume", "click", function() flow.closeAll() end)
    ui.on(flow.docs.pause, "tohub", "click", function()
        flow.closeAll(); showcase.loadLevel("project://Assets/Scenes/Hub.oxscene")
    end)
    ui.on(flow.docs.pause, "quicksave", "click", function() flow.closeAll(); flow.quickSave() end)
    ui.on(flow.docs.pause, "load", "click", function() refreshSaves(); flow.push(flow.docs.saves) end)
    ui.on(flow.docs.pause, "settings", "click", function() flow.push(flow.docs.settings) end)
    ui.on(flow.docs.pause, "mainmenu", "click", function()
        flow.closeAll(); showcase.loadLevel("project://Assets/Scenes/MainMenu.oxscene")
    end)
    ui.on(flow.docs.pause, "quit", "click", function() showcase.quit() end)

    -- Save browser (clicks on generated slot rows bubble to #slots).
    ui.on(flow.docs.saves, "slots", "click", function(ev)
        local s = slotFromTarget(ev.target)
        if not s then return end
        if deleteMode then
            showcase.deleteSave(s.slot)
            refreshSaves()
        else
            flow.closeAll(); flow.inMainMenu = false
            showcase.load(s.slot)
        end
    end)
    ui.on(flow.docs.saves, "savenew", "click", function()
        if flow.inMainMenu then
            ui.setText(flow.docs.saves, "status", "Сначала начните экскурсию.")
            return
        end
        local slot = "manual_" .. os.date("%Y%m%d_%H%M%S")
        showcase.save(slot, "Ручное сохранение · " .. levelName(showcase.currentLevel()))
        timer.after(0.2, refreshSaves)
    end)
    ui.on(flow.docs.saves, "deletemode", "click", function() deleteMode = not deleteMode; refreshSaves() end)
    ui.on(flow.docs.saves, "back", "click", function() deleteMode = false; flow.pop() end)
    ui.on(flow.docs.settings, "back", "click", function() flow.pop() end)

    events.subscribe("showcase.loaded", function(_, p) flow.toast("Загружено: " .. (p and p.slot or "")) end)
    events.subscribe("showcase.loadFailed", function(_, p) flow.toast("Не удалось загрузить: " .. (p and p.error or "")) end)
    events.subscribe("showcase.caption", function(_, p)
        if not hud or not p then return end
        hud:set("captionVisible", true)
        hud:set("captionTitle", p.title or "")
        hud:set("captionText", p.text or "")
        local idx = tonumber(p.index) or 0
        hud:set("captionStep", idx == 0 and "ЭКСКУРСИЯ · ХАБ" or string.format("ЭКСКУРСИЯ · СТАНЦИЯ %d ИЗ %s", idx, p.count or "?"))
    end)
end

-- ---------------------------------------------------------------------------------------------------- per frame
function flow.quickSave()
    if flow.inMainMenu then return end
    showcase.quickSave()
    flow.toast("Быстрое сохранение (F5)")
end

function flow.quickLoad()
    if showcase.quickLoad() then flow.toast("Быстрая загрузка (F9)") else flow.toast("Нет быстрого сохранения") end
end

function flow.openMainMenu()
    flow.inMainMenu = true
    stack = {}
    menuModel:set("canContinue", flow.newestSave() ~= nil)
    flow.push(flow.docs.menu)
end

function flow.update(dt)
    if not hud then return end
    if toastUntil > 0 and now() > toastUntil then
        hud:set("toast", "")
        toastUntil = 0
    end
    local since, kind = showcase.saveIndicator()
    if since < 2.5 then
        hud:set("saving", kind == "auto" and "Автосохранение…" or "Сохранено")
    else
        hud:set("saving", "")
    end
    if not flow.isPlayMode() then return end
    if input.triggered("Menu") then
        if flow.inMainMenu then
            if #stack > 1 then flow.pop() end
        elseif #stack > 0 then
            flow.pop()
        else
            ui.setText(flow.docs.pause, "where", levelName(showcase.currentLevel()))
            flow.push(flow.docs.pause)
        end
    end
    if not flow.inMainMenu and #stack == 0 then
        if input.triggered("QuickSave") then flow.quickSave() end
        if input.triggered("QuickLoad") then flow.quickLoad() end
    end
end

return flow

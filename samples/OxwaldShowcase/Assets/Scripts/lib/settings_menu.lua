-- Extra rows of the settings menu (UI/settings.rml): per-group scalability levels, upscaler quality, screen
-- percentage and key rebinding. The overall quality (Low…Ultra/Auto), RT, upscaler, window, vsync and volumes come
-- from the engine's own "settings" data model.
local M = {}

local groups = {
    { "ViewDistance", "Дальность" }, { "AntiAliasing", "Сглаживание" }, { "Shadows", "Тени" },
    { "GlobalIllumination", "GI и AO" }, { "Reflections", "Отражения" }, { "PostProcess", "Постобработка" },
    { "Textures", "Текстуры" }, { "Effects", "Эффекты" }, { "Foliage", "Растительность" },
    { "Shading", "Шейдинг" }, { "Volumetrics", "Волюметрика" },
}
local levels = { "Low", "Medium", "High", "Ultra" }
local upscalerModes = { "UltraPerformance", "Performance", "Balanced", "Quality", "Native" }
local upscalerNames = { "Ультра-произв.", "Произв.", "Баланс", "Качество", "Натив (DLAA)" }
local percentages = { 50, 67, 75, 85, 100 }

-- Keyboard bindings that can be rebound (context, action, binding index within the action).
local bindings = {
    { "Вперёд", "Move", 2 }, { "Назад", "Move", 3 }, { "Влево", "Move", 1 }, { "Вправо", "Move", 0 },
    { "Прыжок", "Jump", 0 }, { "Бег", "Sprint", 0 }, { "Действие", "Interact", 0 }, { "Меню", "Menu", 0 },
    { "Инфо-панель", "Info", 0 },
}

local flow
local doc
local capturing = nil

local function keyLabel(source)
    if not source or source == "" then return "—" end
    return (source:gsub("^Key%.", ""):gsub("^Mouse%.", "Мышь "):gsub("^Gamepad%.", "Пад "))
end

function M.refresh()
    local rml = {}
    for _, g in ipairs(groups) do
        local cur = tonumber(showcase.cvar("sg." .. g[1]) or "2") or 2
        local row = { '<div class="grp"><span class="gl">', g[2], '</span>' }
        for i, l in ipairs(levels) do
            row[#row + 1] = string.format('<button class="choice small%s" id="g-%s-%d">%s</button>',
                cur == i - 1 and " selected" or "", g[1], i - 1, l)
        end
        row[#row + 1] = "</div>"
        rml[#rml + 1] = table.concat(row)
    end
    ui.setText(doc, "groups", table.concat(rml))

    local q = showcase.cvar("r.Upscaler.Quality") or "Quality"
    local sp = tonumber(showcase.cvar("r.ScreenPercentage") or "100") or 100
    local up = { '<div class="row"><span class="label">Режим апскейла</span><div class="choices">' }
    for i, m in ipairs(upscalerModes) do
        up[#up + 1] = string.format('<button class="choice%s" id="uq-%d">%s</button>', q == m and " selected" or "", i, upscalerNames[i])
    end
    up[#up + 1] = '</div></div><div class="row"><span class="label">Масштаб рендера</span><div class="choices">'
    for i, p in ipairs(percentages) do
        up[#up + 1] = string.format('<button class="choice%s" id="sp-%d">%d%%</button>', math.abs(sp - p) < 0.5 and " selected" or "", i, p)
    end
    up[#up + 1] = "</div></div>"
    ui.setText(doc, "upscalerQuality", table.concat(up))

    local keys = {}
    for i, b in ipairs(bindings) do
        local label = capturing == i and "нажмите клавишу…" or keyLabel(showcase.input.binding("OnFoot", b[2], b[3]))
        keys[#keys + 1] = string.format('<div class="bindrow"><span class="bl">%s</span><button class="small" id="bind-%d">%s</button></div>',
            b[1], i, label)
    end
    ui.setText(doc, "controls", table.concat(keys))
end

function M.init(f)
    flow = f
    doc = flow.docs.settings
    ui.on(doc, "groups", "click", function(ev)
        local g, l = (ev.target or ""):match("^g%-(%a+)%-(%d)$")
        if g then
            showcase.setCVar("sg." .. g, l)
            M.refresh()
        end
    end)
    ui.on(doc, "upscalerQuality", "click", function(ev)
        local t = ev.target or ""
        local uq = tonumber(t:match("^uq%-(%d)$"))
        local sp = tonumber(t:match("^sp%-(%d)$"))
        if uq then showcase.setCVar("r.Upscaler.Quality", upscalerModes[uq]) end
        if sp then showcase.setCVar("r.ScreenPercentage", percentages[sp]) end
        M.refresh()
    end)
    ui.on(doc, "controls", "click", function(ev)
        local i = tonumber((ev.target or ""):match("^bind%-(%d+)$"))
        if not i then return end
        capturing = i
        M.refresh()
        local b = bindings[i]
        showcase.input.capture("OnFoot", b[2], b[3], function(source)
            capturing = nil
            if source ~= "Key.Escape" then flow.toast(b[1] .. " → " .. keyLabel(source)) end
            M.refresh()
        end)
    end)
    ui.on(doc, "resetkeys", "click", function()
        showcase.input.reset()
        M.refresh()
        flow.toast("Клавиши сброшены")
    end)
    M.refresh()
end

return M

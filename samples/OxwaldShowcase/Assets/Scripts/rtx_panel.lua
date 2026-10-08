-- RTX и апскейлеры: ray tracing toggles (greyed with the reason when the GPU cannot trace rays), upscaler switcher
-- with availability, screen percentage and live GPU timings of the render graph passes.
local flow = require("lib.flow")
local doc = flow.docs.side

local effects = {
    { key = "Num1", label = "RT-тени", cvar = "r.RayTracing.Shadows", rt = "Shadows" },
    { key = "Num2", label = "RT-отражения", cvar = "r.RayTracing.Reflections", rt = "Reflections" },
    { key = "Num3", label = "RT GI (DDGI)", cvar = "r.RayTracing.GI", rt = "GlobalIllumination" },
    { key = "Num4", label = "RT AO", cvar = "r.RayTracing.AO", rt = "AmbientOcclusion" },
    { key = "Num5", label = "Path tracer", cvar = "r.PathTracing", rt = "PathTracer" },
}
local upscalers = { "Off", "FSR1", "TAAU", "DLSS" }
local percentages = { 50, 67, 75, 100 }

local function esc(s) return (tostring(s or ""):gsub("<", "&lt;"):gsub(">", "&gt;")) end
-- Shortens long availability reasons for the panel ("DLSS (NVIDIA NGX) is only available …" → first clause).
local function short(s)
    s = tostring(s or "")
    local cut = s:find(" %(") or s:find("; ")
    if cut and cut > 20 then s = s:sub(1, cut - 1) end
    return s
end

local function availability(info, name)
    if not info then return false, "нет данных рендера" end
    for _, e in ipairs(info.rtEffects or {}) do
        if e.name == name or e.cvar:find(name, 1, true) then return e.available, e.reason end
    end
    return info.rtAvailable, info.rtReason
end

local function upscalerInfo(info, name)
    if name == "TAAU" or name == "Off" then return true, "" end
    for _, u in ipairs(info and info.upscalers or {}) do
        if u.name == name then return u.available, u.reason end
    end
    return false, "нет данных"
end

function onStart(self)
    self.visible = true
    self.timer = 0
    ui.show(doc)
end

local function toggleLine(label, key, on, available, reason)
    local cls = available and (on and "toggle" or "toggle off") or "toggle disabled"
    local state = available and (on and "ВКЛ" or "выкл") or "—"
    local line = string.format('<div class="%s"><span class="state">%s</span>[%s] %s</div>', cls, state, key, label)
    if not available and reason and reason ~= "" then line = line .. '<div class="note">' .. esc(reason) .. '</div>' end
    return line
end

function onUpdate(self, dt)
    local info = showcase.renderInfo()
    if showcase.mode() == "play" and not flow.blocking() then
        if input.keyPressed("Tab") then
            self.visible = not self.visible
            if self.visible then ui.show(doc) else ui.hide(doc) end
        end
        if input.keyPressed("R") then
            local ok = info and info.rtAvailable
            if ok then
                showcase.setCVar("r.RayTracing", showcase.cvar("r.RayTracing") ~= "true")
            else
                flow.toast("Трассировка лучей недоступна: " .. (info and info.rtReason or "?"), 4)
            end
        end
        for _, e in ipairs(effects) do
            if input.keyPressed(e.key) then
                local ok, why = availability(info, e.rt)
                if ok then
                    local on = showcase.cvar(e.cvar)
                    showcase.setCVar(e.cvar, on ~= "true" and on ~= "1")
                else
                    flow.toast(e.label .. " недоступно: " .. (why or ""), 4)
                end
            end
        end
        if input.keyPressed("U") then
            local cur = showcase.cvar("r.Upscaler") or "Off"
            local idx = 1
            for i, n in ipairs(upscalers) do if n == cur then idx = i end end
            for step = 1, #upscalers do
                local n = upscalers[(idx - 1 + step) % #upscalers + 1]
                local ok, why = upscalerInfo(info, n)
                if ok then
                    showcase.setCVar("r.Upscaler", n)
                    flow.toast("Апскейлер: " .. n)
                    break
                else
                    flow.toast(n .. " недоступен: " .. why, 3)
                end
            end
        end
        if input.keyPressed("LeftBracket") or input.keyPressed("RightBracket") then
            local sp = tonumber(showcase.cvar("r.ScreenPercentage") or "100") or 100
            local idx = #percentages
            for i, p in ipairs(percentages) do if math.abs(p - sp) < 1 then idx = i end end
            idx = math.clamp(idx + (input.keyPressed("RightBracket") and 1 or -1), 1, #percentages)
            showcase.setCVar("r.ScreenPercentage", percentages[idx])
        end
    end
    self.timer = self.timer - dt
    if self.timer > 0 or not self.visible then return end
    self.timer = 0.3
    local rows = { "<h2>RTX и апскейлеры</h2>" }
    if info then
        rows[#rows + 1] = string.format('<div class="kv"><span class="k">GPU</span><span class="v">%s</span></div>', esc(info.gpu))
        rows[#rows + 1] = toggleLine("Трассировка лучей", "R", showcase.cvar("r.RayTracing") == "true", info.rtAvailable, info.rtReason)
        for _, e in ipairs(effects) do
            local ok, why = availability(info, e.rt)
            local on = showcase.cvar(e.cvar)
            rows[#rows + 1] = toggleLine(e.label, e.key:gsub("Num", ""), on == "true" or on == "1", ok, nil)
        end
        local cur = showcase.cvar("r.Upscaler") or "Off"
        local ups = {}
        for _, n in ipairs(upscalers) do
            local ok, why = upscalerInfo(info, n)
            local cls = n == cur and "toggle" or (ok and "toggle off" or "toggle disabled")
            ups[#ups + 1] = string.format('<div class="%s"><span class="state">%s</span>%s%s</div>', cls,
                n == cur and "●" or (ok and "○" or "✕"), n, ok and "" or (' <span class="muted">— ' .. esc(short(why)) .. '</span>'))
        end
        rows[#rows + 1] = '<h2 style="margin-top: 10dp;">[U] Апскейлер</h2>' .. table.concat(ups)
        rows[#rows + 1] = string.format('<div class="kv"><span class="k">[ ] Рендер / вывод</span><span class="v">%dx%d → %dx%d</span></div>',
            info.renderWidth, info.renderHeight, info.outputWidth, info.outputHeight)
        local st = showcase.stats()
        rows[#rows + 1] = string.format('<h2 style="margin-top: 10dp;">GPU %.2f мс</h2>', st.gpuMs or 0)
        local passes = showcase.gpuPasses(7)
        local maxMs = 0.01
        for _, p in ipairs(passes) do maxMs = math.max(maxMs, p.ms) end
        for _, p in ipairs(passes) do
            rows[#rows + 1] = string.format('<div class="pass"><span class="pn">%s</span><span class="bar" style="width: %ddp;"></span> %.2f</div>',
                esc(p.name):sub(1, 26), math.floor(p.ms / maxMs * 90), p.ms)
        end
        if not info.rtAvailable then
            rows[#rows + 1] = '<div class="note">На RTX-видеокарте переключатели работают на лету, без перезапуска.</div>'
        end
    else
        rows[#rows + 1] = '<div class="note">Ожидание данных рендера…</div>'
    end
    ui.setText(doc, "content", table.concat(rows))
end

function onDestroy(self) ui.hide(doc) end

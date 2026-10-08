-- Сеть: on-screen net stats of the listen-server demo; [+]/[-] latency, [L] packet loss.
local flow = require("lib.flow")
local doc = flow.docs.side

function onStart(self)
    self.latency = 60
    self.loss = 5
    self.timer = 0
    ui.show(doc)
    showcase.net.setConditions(self.latency, self.loss, 10)
end

local function kv(k, v) return string.format('<div class="kv"><span class="k">%s</span><span class="v">%s</span></div>', k, v) end

function onUpdate(self, dt)
    if showcase.mode() == "play" and not flow.blocking() then
        local changed = false
        if input.keyPressed("Equal") or input.keyPressed("KpAdd") then self.latency = math.min(400, self.latency + 40); changed = true end
        if input.keyPressed("Minus") or input.keyPressed("KpSubtract") then self.latency = math.max(0, self.latency - 40); changed = true end
        if input.keyPressed("L") then self.loss = self.loss >= 20 and 0 or self.loss + 5; changed = true end
        if changed then
            showcase.net.setConditions(self.latency, self.loss, 10)
            flow.toast(string.format("Канал: задержка %d мс, потери %d%%", self.latency, self.loss))
        end
    end
    self.timer = self.timer - dt
    if self.timer > 0 then return end
    self.timer = 0.25
    local s = showcase.net.stats()
    local rows = {
        "<h2>Сеть · listen-сервер + бот</h2>",
        kv("Состояние", s.running and (s.connected and "подключён" or "подключение…") or "не запущен"),
        kv("Транспорт", "in-memory (MemoryNetwork)"),
        kv("Задержка / потери", string.format("%d мс / %d%%", s.latencyMs, s.lossPercent)),
        kv("RTT", string.format("%.0f мс", s.rttMs)),
        kv("Тикрейт сервера", string.format("%.0f Гц", s.tickRate)),
        kv("Снапшот", string.format("%d байт", s.snapshotBytes)),
        kv("Приём бота", string.format("%.1f КБ/с", s.receiveKBps)),
        kv("Отправка бота", string.format("%.1f КБ/с", s.sendKBps)),
        kv("Реплик у клиента", tostring(s.objects)),
        kv("Буфер интерполяции", string.format("%.0f мс", s.interpolationDelayMs)),
        kv("Отставание призрака", string.format("%.0f см", s.errorCm)),
        '<div class="note">Голубые полупрозрачные копии — то, что видит бот-клиент: репликация снапшотов 30 Гц и интерполяция на ~100 мс позади сервера.</div>',
    }
    ui.setText(doc, "content", table.concat(rows))
end

function onDestroy(self) ui.hide(doc) end

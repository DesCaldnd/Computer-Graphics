-- Разрушаемость: [ЛКМ]/[E] — ядро из камеры, [Q] — взрыв в точке прицела, [G] — граф опор (жёлтые связи, зелёные
-- опоры), [T] — демо-сценарий, [R] — собрать станцию заново. В экскурсии и фото-режиме демо запускается само.
local flow = require("lib.flow")

properties = {
    speed = { type = "float", default = 34, tooltip = "Скорость ядра, м/с" },
    maxBalls = { type = "int", default = 6, tooltip = "Ядер на сцене одновременно" },
    blastRadius = { type = "float", default = 1.8 },
    blastDamage = { type = "float", default = 1000 },
    blastImpulse = { type = "float", default = 150, tooltip = "Импульс, Н·с, для обломков в радиусе" },
}

-- Демо: время (с) → действие. Колонна ломается — балка крошится над ней; взрыв у основания башни — башня
-- обрушивается; ядро в стену — дыра; ядро в ящики.
local demo = {
    { 0.6, "shoot", "Column.L", vec3(0, -0.4, 0) },
    { 1.3, "blast", "Tower", vec3(0, -2.5, 0) },
    { 2.2, "shoot", "Wall", vec3(-1.2, -0.6, 0) },
    { 3.0, "shoot", "Crate", vec3(0, 0, 0) },
}

local function fire(self, origin, dir)
    local ball = scene.spawn("Cannonball", origin)
    if not ball then return end
    ball:get("RigidBody").initialLinearVelocity = dir * self.speed
    self.balls[#self.balls + 1] = ball
    if #self.balls > self.maxBalls then
        local old = table.remove(self.balls, 1)
        if old:isValid() then scene.destroy(old) end
    end
    audio.play("project://Assets/Audio/raygun.wav", origin, 0.6)
end

local function blast(self, center)
    local n = destruction.explode(center, self.blastRadius, self.blastDamage, self.blastImpulse)
    audio.play("project://Assets/Audio/crumble.wav", center, 1.0)
    return n
end

local function runDemoStep(self, step)
    local target = scene.find(step[3])
    if not target then return end
    local p = target.transform.worldPosition + step[4]
    if step[2] == "shoot" then
        local muzzle = self.cannon and self.cannon.transform.worldPosition or vec3(0, 1.2, 6)
        fire(self, muzzle, (p - muzzle):normalized())
    else
        blast(self, p)
    end
end

function onStart(self)
    self.balls = {}
    self.time = 0
    self.next = 1
    self.cannon = scene.find("Cannon")
    self.cam = scene.find("PlayerCamera")
    self.debug = false
    self.lastDebris = 0
    self.statusTimer = 0
    self.autoDemo = showcase.mode() ~= "play"
end

function onUpdate(self, dt)
    self.time = self.time + dt
    if self.autoDemo and self.next <= #demo and self.time >= demo[self.next][1] then
        runDemoStep(self, demo[self.next])
        self.next = self.next + 1
    end
    if showcase.mode() ~= "play" or flow.blocking() then return end

    local t = self.cam and self.cam.transform
    if t and (input.triggered("Fire") or input.triggered("Interact")) then
        fire(self, t.worldPosition + t.forward * 1.0, t.forward)
    end
    if t and input.keyPressed("Q") then
        local hit = physics.raycast(t.worldPosition, t.forward, 80, scene.find("Player"))
        if hit then
            local n = blast(self, hit.point)
            flow.toast(string.format("Взрыв: задето %d объектов", n), 1.5)
        end
    end
    if input.keyPressed("G") then
        self.debug = not self.debug
        destruction.setDebugDraw(self.debug)
        flow.toast(self.debug and "Граф опор: жёлтые связи, зелёные — опорные куски" or "Граф опор скрыт", 2)
    end
    if input.keyPressed("T") then
        self.autoDemo = true
        self.time = 0
        self.next = 1
        flow.toast("Демо-сценарий")
    end
    if input.keyPressed("R") then showcase.loadLevel(showcase.currentLevel()) end

    -- Debris budget readout when the number of moving chunks changes noticeably.
    self.statusTimer = self.statusTimer + dt
    if self.statusTimer > 0.5 then
        self.statusTimer = 0
        local n = destruction.activeDebris()
        if math.abs(n - self.lastDebris) >= 4 then
            flow.toast(string.format("Обломков в движении: %d (бюджет %s)", n, showcase.cvar("destruction.MaxActiveDebris") or "?"), 1.5)
            self.lastDebris = n
        end
    end
end

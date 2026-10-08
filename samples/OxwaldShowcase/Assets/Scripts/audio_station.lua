-- Звук: sets up the mixer of the station (ducking: Voice → Music, reverb on the "Cave" bus) and plays the
-- announcement at the speaker tower; the speaker rings pulse with the bus levels.
local interact = require("lib.interact")
local flow = require("lib.flow")

function onStart(self)
    showcase.audio.reverb("Cave", 0.0, 0.9)      -- created dry; the tunnel trigger raises the wet level
    showcase.audio.duck("Voice", "Music", 0.2)
    self.announcer = scene.find("Announcer")
    self.button = scene.find("AnnounceButton")
    self.rings = {}
    for _, e in ipairs(scene.findAll("MeshRenderer")) do
        if e.name == "SpeakerRing" then self.rings[#self.rings + 1] = e end
    end
    self.proxy = { entity = self.button }
    self.timer = 6
    -- Tour / photo mode: draw the sources' min/max distance rings and the listener lines.
    if showcase.mode() ~= "play" then showcase.debugDraw("audio", true) end
end

local function announce(self)
    audio.playSource(self.announcer)
    flow.toast("Объявление: музыка приглушается (ducking Voice → Music)")
end

function onUpdate(self, dt)
    if self.button then interact.update(self.proxy, 2.2, "Объявление по громкой связи", function() announce(self) end) end
    -- Automatic announcement every 20 s so the ducking is audible without interaction.
    self.timer = self.timer - dt
    if self.timer <= 0 then self.timer = 20; announce(self) end
    local level = showcase.audio.level("Music")
    local s = 0.7 + math.min(0.4, level * 4)
    for _, r in ipairs(self.rings) do r.transform.scale = vec3(s, s, 0.2) end
end

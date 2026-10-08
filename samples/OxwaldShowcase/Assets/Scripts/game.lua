-- Game controller of every scene (entity "Game"): station HUD + info panel, menus, quick save/load, tour captions.
-- Station texts come from the properties the generator wrote (Tools/gen_common.cpp, SceneBuilder::game).
local flow = require("lib.flow")

properties = {
    station = { type = "string", default = "hub", tooltip = "Station id (stations.json)" },
    title = { type = "string", default = "" },
    description = { type = "string", default = "" },
    hints = { type = "string", default = "" },
    guide = { type = "string", default = "" },
    menu = { type = "bool", default = false, tooltip = "Main menu scene" },
}

local function stationNumber(id)
    for i, s in ipairs(showcase.stations()) do
        if s.id == id then return i end
    end
    return nil
end

function onCreate(self)
    flow.init()
    self.infoTimer = 0
    self.infoPinned = false
    self.fpsTimer = 0
end

function onStart(self)
    local hud = flow.hud()
    local mode = showcase.mode()
    self.board = scene.find("InfoBoard")
    self.player = scene.find("Player")
    hud:set("captionVisible", false)
    hud:set("prompt", "")
    if self.menu then
        hud:set("badgeVisible", false)
        hud:set("infoVisible", false)
        hud:set("crosshair", false)
        if mode == "play" then flow.openMainMenu()
        elseif mode == "photo" then ui.show(flow.docs.menu) end -- screenshot of the menu
        return
    end
    flow.inMainMenu = false
    if mode == "play" then flow.closeAll() end
    local n = stationNumber(self.station)
    hud:set("badgeVisible", true)
    hud:set("stationNum", n and string.format("СТАНЦИЯ %02d / %02d", n, #showcase.stations()) or "OXWALD SHOWCASE")
    hud:set("stationTitle", self.title)
    hud:set("infoTitle", self.title)
    hud:set("infoDesc", self.description)
    hud:set("infoHints", self.hints)
    hud:set("infoGuide", self.guide)
    hud:set("crosshair", mode == "play")
    -- The info panel opens on arrival (always in tour/photo mode: screenshots tell the story).
    self.infoTimer = mode == "play" and 9 or 1e9
    hud:set("infoVisible", true)
    if flow.guided and mode == "play" then
        flow.toast("Экскурсия: подойдите к инфо-стенду, затем к порталу хаба")
    end
end

function onUpdate(self, dt)
    flow.update(dt)
    if self.menu then return end
    local hud = flow.hud()
    -- Info panel: pinned with [I], shown near the board and for a while after arriving.
    if showcase.mode() == "play" and not flow.blocking() then
        if input.triggered("Info") then self.infoPinned = not hud:get("infoVisible"); self.infoTimer = 0 end
        self.infoTimer = math.max(0, self.infoTimer - dt)
        local nearBoard = false
        if self.board and self.player then
            nearBoard = self.board.transform.worldPosition:distance(self.player.transform.worldPosition) < 4.5
        end
        hud:set("infoVisible", self.infoPinned or nearBoard or self.infoTimer > 0)
    end
    self.fpsTimer = self.fpsTimer - dt
    if self.fpsTimer <= 0 then
        self.fpsTimer = 0.5
        local st = showcase.stats()
        if st.fps and st.fps > 0 then
            hud:set("fps", string.format("%.0f FPS · %.1f ms · GPU %.1f ms", st.fps, st.frameMs or 0, st.gpuMs or 0))
        end
    end
end

function onDestroy(self)
    local hud = flow.hud()
    if hud then hud:set("prompt", "") end
end

-- Interactables: shows "[E] …" when the player is close and calls `fn(self)` on the Interact action.
local flow = require("lib.flow")
local M = {}

local owner = nil -- the interactable currently owning the prompt

function M.update(self, radius, text, fn)
    if showcase.mode() ~= "play" or flow.blocking() then return end
    self._player = self._player or scene.find("Player")
    if not self._player then return end
    local d = self.entity.transform.worldPosition:distance(self._player.transform.worldPosition)
    if d < radius then
        owner = self.entity.id
        flow.prompt("[E] " .. text)
        if input.triggered("Interact") then fn(self) end
    elseif owner == self.entity.id then
        owner = nil
        flow.prompt("")
    end
end

return M

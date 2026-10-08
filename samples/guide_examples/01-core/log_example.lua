-- Глава 01: логирование из Lua. log.* пишет в тот же лог, что и OX_LOG_* (категория "script").
log.info("player spawned at", 1, 2, 3)
local hp = 15
if hp < 20 then
    log.warn("hp low: " .. hp)
end
print("print goes to the log too")

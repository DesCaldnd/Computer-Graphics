-- Модуль: require("util.aim") ищет <searchRoot>/util/aim.lua. Результат кэшируется.
local M = {}

-- Угол поворота по горизонтали (градусы) от `from` к `to`; «вперёд» в движке — это -Z.
function M.yawTo(from, to)
    local d = to - from
    return math.deg(math.atan(d.x, -d.z))
end

return M

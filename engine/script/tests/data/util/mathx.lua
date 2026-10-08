local M = {}

function M.round2(x)
    return math.floor(x * 100 + 0.5) / 100
end

return M

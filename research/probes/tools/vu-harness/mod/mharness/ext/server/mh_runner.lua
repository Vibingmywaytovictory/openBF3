-- Scenario scheduler (SKELETON; DESIGN.md section 3.8). One active coroutine resumed per PostSim tick, a FIFO queue,
-- stop/pause/next, timeouts on every wait, ticket guard, level-change handling, cleanup on every exit path.
local M = {}

function M.run(sc, args, player) end
function M.queue(items, player) end
function M.stop(player) end
function M.step() end
function M.status() return '' end

return M

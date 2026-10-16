-- W-A fixed wall and W-A60 (DESIGN.md 6.2, 6.3). SKELETON: state machine per DESIGN.md; run(ctx, args) is a coroutine body resumed once per tick.
local M = { id = 'WA' }

function M.estimate(args) return 0 end
function M.run(ctx, args) end
function M.cleanup(ctx) end

return M

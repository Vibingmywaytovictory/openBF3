-- W-D client (DESIGN.md 6.10). SKELETON: state machine per DESIGN.md; run(ctx, args) is a coroutine body resumed once per tick.
local M = { id = 'WD' }

function M.estimate(args) return 0 end
function M.run(ctx, args) end
function M.cleanup(ctx) end

return M

-- Chat replies and prompts (SKELETON; DESIGN.md sections 3.7, 4 'prompt').
-- ChatManager:SendMessage(message, toPlayer) -> bool and Yell(message, duration, toPlayer)
-- (server/library/chatmanager.md). Prefix '[mh] ', at most 120 chars per message and 6 messages per reply.
-- Every instruction to the user is also a 'prompt' row (machine-readable for the extractor and the mock user).
local M = {}

function M.say(player, text) end
function M.progress(player, text) end
-- prompt(player, kind, fields, text): chat line + prompt row {kind, n, of, wid, zone, d, id, dir, wait}.
function M.prompt(player, kind, fields, text) end

return M

-- Chat replies and prompts (SKELETON; DESIGN.md sections 3.7, 4 'prompt').
-- ChatManager:SendMessage(message, toPlayer) -> bool and Yell(message, duration, toPlayer)
-- (server/library/chatmanager.md). Prefix '[mh] ' included in the cap: at most 120 chars per message
-- (SendMessage limit 127), 6 messages per reply. cfg-done is echoed to chat (crash-resume hint).
-- Every instruction to the user is also a 'prompt' row (machine-readable for the extractor and the mock user).
local M = {}

function M.say(player, text) end
function M.progress(player, text) end
-- prompt(player, kind, fields, text): chat line + prompt row {kind, n, of, wid, zone, d, id, dir, wait}.
function M.prompt(player, kind, fields, text) end

return M

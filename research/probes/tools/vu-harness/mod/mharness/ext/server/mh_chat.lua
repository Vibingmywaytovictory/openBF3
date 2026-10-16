-- Chat replies (SKELETON; DESIGN.md section 3.7). ChatManager:SendMessage(msg, player) and Yell(msg, s, player)
-- (server/library/chatmanager.md). Prefix '[mh] ', at most 120 chars per message and 6 messages per reply.
local M = {}

function M.say(player, text) end
function M.progress(player, text) end

return M

-- '!mh' command parser and table (SKELETON; DESIGN.md sections 3.7, 5).
-- Player:Chat(player, recipientMask, message) (server/event/player_chat.md); client console 'mh' via NetEvent 'mh:cmd'.
-- Tokens [A-Za-z0-9_.,:=+-]{1,32}, at most 12; every command emits a 'cmd' row; every verb is repeat-safe.
local M = {}

M.verbs = {
	'help', 'status', 'census', 'p0', 'wall', 'range', 'flat', 'ring', 'sites', 'run', 'queue', 'stop', 'pause', 'resume',
	'next', 'mark', 'ok', 'target', 'untarget', 'clear', 'watch', 'unwatch', 'rate', 'nextmap', 'restartround',
}

function M.on_chat(player, recipientMask, message) end

return M

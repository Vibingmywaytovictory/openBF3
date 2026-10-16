-- Event rows (SKELETON; DESIGN.md section 3.10). server/event/*.md and shared/event/level_destroy.md.
local M = {}

M.EVENTS = {
	'Player:Killed', 'Player:Respawn', 'Player:Reload', 'Player:SuppressedEnemy', 'Player:EnteredCapturePoint',
	'Player:ExitedCapturePoint', 'CapturePoint:Captured', 'CapturePoint:Lost', 'Soldier:HealthAction', 'Soldier:ManDown',
	'Server:RoundOver', 'Server:RoundReset',
}

function M.subscribe() end

return M

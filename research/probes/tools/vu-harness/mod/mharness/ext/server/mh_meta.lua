-- meta and census rows (SKELETON; DESIGN.md sections 4, 6.1).
-- Level:Loaded (server/event/level_loaded.md); SharedUtils:GetTickrate (shared/library/sharedutils.md);
-- RCON:SendCommand for vu.FrequencyMode, whitelisted vars and modList.ListRunning (server/library/rcon.md);
-- census via EntityManager:TraverseAllEntities (shared/library/entitymanager.md). The password variable is never read.
local M = {}

M.VAR_WHITELIST = {
	'vars.regenerateHealth', 'vars.soldierHealth', 'vars.bulletDamage', 'vars.playerRespawnTime', 'vars.gameModeCounter',
	'vars.friendlyFire', 'vars.autoBalance', 'vars.idleTimeout', 'vars.roundStartPlayerCount', 'vu.SuppressionMultiplier',
	'vu.TimeScale', 'vu.DisablePreRound', 'vu.FrequencyMode',
}

function M.on_level_loaded(levelName, gameMode, round, roundsPerMap) end
function M.census(player) end

return M

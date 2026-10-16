-- Small integer ids (SKELETON; DESIGN.md section 3.4). Humans 1-99, bots 101-999, weapons/materials/flags 1..
-- Never reads a human's name, guid, accountGuid, onlineId or ip. Emits 'dict' rows before the first use of an id.
local M = {}

function M.player(player) return nil end
function M.weapon(name) return nil end
-- material id from the hit's MaterialContainerPair (fb/materialcontainerpair.md): {physicsMaterialIndex, physicsPropertyIndex}
function M.material(materialContainer) return nil end
function M.flag(capturePoint) return nil end

return M

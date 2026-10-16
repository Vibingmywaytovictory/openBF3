-- Small integer ids (SKELETON; DESIGN.md section 3.4). Humans 1-99, bots 101-999, weapons/materials/flags 1..
-- Never reads a human's name, guid, accountGuid, onlineId or ip. Emits 'dict' rows before the first use of an id.
local M = {}

function M.player(player) return nil end
function M.weapon(name) return nil end
-- material id: RayCastHit.material is DataContainer|nil (shared/type/raycasthit.md); after a nil check it is cast with
-- MaterialContainerPair(hit.material) (fb/materialcontainerpair.md, read only) -> {flagsAndIndex, physicsMaterialIndex,
-- physicsPropertyIndex}; mapping to our grid is decided after Phase 0 (DESIGN 3.4).
function M.material(materialContainer) return nil end
function M.flag(capturePoint) return nil end

return M

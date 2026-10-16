-- Recorded poses: wall, range, flat, ring (SKELETON; DESIGN.md sections 6.1-6.9).
-- Camera from the client (NetEvent 'mh:camreq' -> 'mh:cam'); RaycastManager:DetailedRaycast
-- (server/library/raycastmanager.md) for the wall plane, flatness and ground snapping. Emits 'site' rows.
local M = {}

function M.record(name, player, args) end
function M.get(name) return nil end

return M

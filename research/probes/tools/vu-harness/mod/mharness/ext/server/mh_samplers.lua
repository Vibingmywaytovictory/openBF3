-- Samplers: hp, tk, cp, wf and the watched set (SKELETON; DESIGN.md section 3.10).
-- TicketManager:GetTicketCount (server/library/ticketmanager.md); ServerTicketCounterEntity and
-- ServerCapturePointEntity iterators (shared/library/entitymanager.md; server/type/ticketcounterentity.md,
-- server/type/capturepointentity.md); GunSway:Update (shared/event/gunsway_update.md).
local M = {}

function M.watch(id, what, until_k) end
function M.unwatch(id) end
function M.on_postsim() end
function M.on_gunsway(gunSway, weapon, weaponFiring, deltaTime) end

return M

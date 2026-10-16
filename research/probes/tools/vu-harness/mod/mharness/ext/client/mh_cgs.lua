-- Client gs rows with src pred/corr (SKELETON; DESIGN.md section 3.11).
-- GunSway:Update (shared/event/gunsway_update.md); SoldierWeapon.predictedWeaponFiring/correctedWeaponFiring
-- (client/type/soldierweapon.md); PlayerManager:GetLocalPlayer (client/library/playermanager.md).
local M = {}

function M.on_gunsway(gunSway, weapon, weaponFiring, deltaTime) end

return M

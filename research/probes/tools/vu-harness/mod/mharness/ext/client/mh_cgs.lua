-- Client gs rows with src pred/corr (DESIGN.md section 3.11).
-- GunSway:Update (shared/event/gunsway_update.md); SoldierWeapon.predictedWeaponFiring/correctedWeaponFiring
-- (client/type/soldierweapon.md); PlayerManager:GetLocalPlayer (client/library/playermanager.md). Rows on fireShot and
-- every 4th frame otherwise (R section 3.3). wid is 0 on the client (weapon dict ids are server-side).
local R = require('__shared/mh_row')
local G = require('__shared/mh_gs')
local C = require('__shared/mh_clock')

local M = {}

M.frame = 0
M.last = {}
M.n = 0

function M.reset()
	M.last = {}
end

local function local_weapon()
	local w, pose, zoom = nil, 0, false
	pcall(function()
		local so = PlayerManager:GetLocalPlayer().soldier
		w = so.weaponsComponent.currentWeapon
		zoom = so.weaponsComponent.isZooming == true
		pose = math.tointeger(so.pose) or 0
	end)
	return w, pose, zoom
end

function M.on_gunsway(g, weapon, wf, dt, rec)
	if wf == nil then return end
	local w, pose, zoom = local_weapon()
	if w == nil then return end
	local iid = wf.instanceId
	local src = nil
	pcall(function()
		if w.predictedWeaponFiring ~= nil and w.predictedWeaponFiring.instanceId == iid then src = 'pred' end
		if w.correctedWeaponFiring ~= nil and w.correctedWeaponFiring.instanceId == iid then src = 'corr' end
	end)
	if src == nil then return end
	local fs = g.fireShot == true
	local key = src .. ':' .. C.gk
	if M.last[src] == C.gk then return end
	if not fs and C.gk % 4 ~= 0 then return end
	M.last[src] = C.gk
	M.n = M.n + 1
	R.row('gs', G.payload(g, rec.pid or 0, 0, src, pose, zoom, nil, dt, nil))
	return key
end

return M

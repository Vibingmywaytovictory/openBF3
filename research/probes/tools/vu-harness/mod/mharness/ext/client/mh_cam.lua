-- Camera transform replies for '!mh wall/range/flat/ring' and 'p0 aim' (DESIGN.md sections 3.9, 3.11).
-- ClientUtils:GetCameraTransform / WorldToScreen / GetWindowSize (client/library/clientutils.md); the forward sign is
-- the one whose projection lands at the screen centre [live:L19]. AimingSimulation.yaw/pitch of the local weapon
-- (client/type/aimingsimulation.md) are sent for the P0.5 cross-check (read only).
local V = require('__shared/mh_vec')

local M = {}

M.sign = nil

-- sign(t): +1 if trans + forward projects to the screen centre, -1 if trans - forward does, else nil.
function M.forward_sign(t)
	local size = ClientUtils:GetWindowSize()
	if size == nil then return nil end
	local cx, cy = size.x / 2, size.y / 2
	local p, f = V.v(t.trans), V.v(t.forward)
	local best, bd = nil, math.huge
	for _, s in ipairs({ 1, -1 }) do
		local sp = ClientUtils:WorldToScreen(V.vec3(V.add(p, V.scale(f, 10 * s))))
		if sp ~= nil then
			local d = math.abs(sp.x - cx) + math.abs(sp.y - cy)
			if d < bd then best, bd = s, d end
		end
	end
	if best and bd < 0.05 * size.y then return best end
	return nil
end

local function aiming()
	local ay, ap = nil, nil
	pcall(function()
		local w = PlayerManager:GetLocalPlayer().soldier.weaponsComponent.currentWeapon
		ay, ap = w.aimingSimulation.yaw, w.aimingSimulation.pitch
	end)
	return ay, ap
end

function M.reply(tag)
	local t = ClientUtils:GetCameraTransform()
	if t == nil then return end
	local s = M.forward_sign(t) or M.sign or 1
	M.sign = s
	local p, f = V.v(t.trans), V.scale(V.v(t.forward), s)
	local ay, ap = aiming()
	NetEvents:Send('mh:cam', { tag = tag, p = p, f = f, sgn = s, ay = ay, ap = ap })
end

return M

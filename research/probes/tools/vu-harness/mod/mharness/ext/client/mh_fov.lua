-- FOV probe on zoom change (DESIGN.md section 3.11; R section 3.3).
-- ClientUtils:GetCameraTransform/WorldToScreen/GetWindowSize (client/library/clientutils.md);
-- AimingSimulation.zoomLevel/fovMultiplier (client/type/aimingsimulation.md) [live:L19].
-- Points 5 degrees about the camera's left and up axes at 100 m are projected; fov = 2 atan(tan 5 deg (size/2) / px).
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local cam = require('mh_cam')

local M = {}

M.last_zoom = nil
M.settle = 0
M.n = 0

function M.reset()
	M.last_zoom = nil
	M.settle = 0
end

local function zoom_state()
	local z, fm = nil, nil
	pcall(function()
		local a = PlayerManager:GetLocalPlayer().soldier.weaponsComponent.currentWeapon.aimingSimulation
		z, fm = math.tointeger(a.zoomLevel) or a.zoomLevel, a.fovMultiplier
	end)
	return z, fm
end

function M.measure(z, fm)
	local t = ClientUtils:GetCameraTransform()
	local size = ClientUtils:GetWindowSize()
	if t == nil or size == nil then return false end
	local s = cam.forward_sign(t) or cam.sign or 1
	local p = V.v(t.trans)
	local f = V.scale(V.v(t.forward), s)
	local l, u = V.v(t.left), V.v(t.up)
	local a = math.rad(5)
	local c = ClientUtils:WorldToScreen(V.vec3(V.add(p, V.scale(f, 100))))
	local ph = ClientUtils:WorldToScreen(V.vec3(V.add(p, V.scale(V.add(V.scale(f, math.cos(a)), V.scale(l, math.sin(a))), 100))))
	local pv = ClientUtils:WorldToScreen(V.vec3(V.add(p, V.scale(V.add(V.scale(f, math.cos(a)), V.scale(u, math.sin(a))), 100))))
	if c == nil or ph == nil or pv == nil then return false end
	local dx, dy = math.abs(ph.x - c.x), math.abs(pv.y - c.y)
	if dx < 1e-6 or dy < 1e-6 then return false end
	local tan5 = math.tan(a)
	local hfov = math.deg(2 * math.atan(tan5 * (size.x / 2) / dx))
	local vfov = math.deg(2 * math.atan(tan5 * (size.y / 2) / dy))
	M.n = M.n + 1
	R.row('fov', { zoom = z or 0, vfov = vfov, hfov = hfov, w = math.tointeger(size.x) or size.x,
		h = math.tointeger(size.y) or size.y, fovmul = fm, sgn = s })
	return true
end

-- on_frame(rec): measure 10 frames after each zoom level change (the zoom transition settles first).
function M.on_frame(rec)
	local z, fm = zoom_state()
	if z == nil then return end
	if z ~= M.last_zoom then
		M.last_zoom = z
		M.settle = 10
		return
	end
	if M.settle > 0 then
		M.settle = M.settle - 1
		if M.settle == 0 then M.measure(z, fm) end
	end
end

return M

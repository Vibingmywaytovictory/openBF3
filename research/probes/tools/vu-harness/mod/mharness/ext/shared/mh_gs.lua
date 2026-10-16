-- GunSway -> 'gs' payload (DESIGN.md sections 3.10, 4). One field list for server and client.
-- GunSway fields: shared/type/gunsway.md; GunSwayDeviation {pitch, yaw, roll, transY}: shared/type/gunswaydeviation.md.
-- Units stay raw (da, rec, ra, rr) until P0.11 decides them [live:L11].
local M = {}

local function dev2(d)
	if d == nil then return { 0, 0 } end
	return { d.pitch, d.yaw }
end

local function dev4(d)
	if d == nil then return { 0, 0, 0, 0 } end
	return { d.pitch, d.yaw, d.roll, d.transY }
end

local function b(x) return x == true end

-- payload(gunSway, id, wid, src, pose, zoom, nup, dt, fl) -> the gs row payload (keys of schema rows_v1 'gs').
function M.payload(g, id, wid, src, pose, zoom, nup, dt, fl)
	local p = {
		id = id, wid = wid, src = src,
		da = g.dispersionAngle, dmin = g.minDispersionAngle,
		rec = dev4(g.currentRecoilDeviation), disp = dev2(g.currentDispersionDeviation),
		lag = dev2(g.currentLagDeviation),
		ra = g.randomAngle, rr = g.randomRadius, seed = math.tointeger(g.seed) or 0,
		ifs = math.tointeger(g.initialFireShot) or 0, fs = b(g.fireShot), fir = b(g.isFiring),
		tsls = g.timeSinceLastShot, mov = b(g.isMoving), jmp = b(g.isJumping), jt = g.jumpingTimer,
		gsf = g.currentGameplayDeviationScaleFactor, vsf = g.currentVisualDeviationScaleFactor,
		smf = g.suppressionMinDispersionAngleFactor, chf = g.crossHairDispersionFactor,
		pose = pose or 0, zoom = zoom == true,
	}
	if nup then p.nup = nup end
	if dt then p.dt = dt end
	if fl then p.fl = fl end
	return p
end

return M

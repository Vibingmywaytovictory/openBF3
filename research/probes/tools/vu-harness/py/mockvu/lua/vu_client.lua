-- Mock VU client realm (DESIGN.md 13.2): the client libraries the mod uses, the local player built from the server's
-- per-tick snapshots of the human (camera = eye, forward = aim), a pinhole projection with a vertical FOV per zoom
-- level on a 2560 x 1440 window [mock], the local weapon's predicted/corrected WeaponFiring with GunSway:Update per
-- frame, Console:Register, and the frame loop driven by the host (mock.frames).
mock = mock or {}
local M = mock
local A = M.A

M.realm = 'client'
M.WIN = { 2560, 1440 }
M.VFOV = { [0] = 55.0, [1] = 40.0, [2] = 20.0 }  -- vertical FOV per zoom level (deg) [mock]
M.console = {}       -- name -> {fn, group}
M.snap = nil
M.local_pid = -1
M.sources = nil

-- Local player objects ------------------------------------------------------------------------------------------------
local LP = { __cls = 'Player', __owner = 'human', id = -1 }
local LSO = { __cls = 'SoldierEntity', __owner = 'human' }
local LWC = { __cls = 'SoldierWeaponsComponent', __owner = 'human' }
local LW = { __cls = 'SoldierWeapon', __owner = 'human' }
local LAIM = { __cls = 'AimingSimulation', __owner = 'human', yaw = 0.0, pitch = 0.0, zoomLevel = 0, fovMultiplier = 1.0 }
local function dev() return { __cls = 'GunSwayDeviation', __owner = 'none', pitch = 0.0, yaw = 0.0, roll = 0.0, transY = 0.0 } end
local function new_gs()
	return { __cls = 'GunSway', __owner = 'none', dispersionAngle = 0.0, minDispersionAngle = 0.0, timeSinceLastShot = 10.0,
		crossHairDispersionFactor = 1.0, currentGameplayDeviationScaleFactor = 1.0, currentVisualDeviationScaleFactor = 1.0,
		suppressionMinDispersionAngleFactor = 1.0, randomAngle = 0.0, randomRadius = 0.0, jumpingTimer = 0.0, seed = 0,
		initialFireShot = 0, fireShot = false, isFiring = false, isJumping = false, isMoving = false,
		isSupportedShooting = false, dev_rec = dev(), dev_disp = dev(), dev_lag = dev(),
		__get = {
			currentRecoilDeviation = function(o) return M.px(o.dev_rec, 'GunSwayDeviation') end,
			currentDispersionDeviation = function(o) return M.px(o.dev_disp, 'GunSwayDeviation') end,
			currentLagDeviation = function(o) return M.px(o.dev_lag, 'GunSwayDeviation') end,
			cameraRecoilTransform = function() return M.lt() end,
		} }
end
local PRED = { __cls = 'WeaponFiring', __owner = 'none', instanceId = 91001, gs = new_gs() }
local CORR = { __cls = 'WeaponFiring', __owner = 'none', instanceId = 91002, gs = new_gs() }
for _, wf in ipairs({ PRED, CORR }) do
	wf.weaponState = 3
	wf.lastWeaponState = 3
	wf.nextWeaponState = 3
	wf.timeToWait = 0.0
	wf.recoilTimer = 0.0
	wf.recoilAngleX, wf.recoilAngleY, wf.recoilAngleZ, wf.recoilFovAngle = 0.0, 0.0, 0.0, 0.0
	wf.primaryAmmoToFill = 0
	wf.__get = { gunSway = function(o) return M.px(o.gs, 'GunSway') end }
end

LP.__get = {
	soldier = function(o) return (M.snap and M.snap.alive) and M.px(LSO, 'SoldierEntity') or nil end,
	alive = function() return M.snap ~= nil and M.snap.alive == true end,
	teamId = function() return M.snap and M.snap.team or 0 end,
	id = function() return M.local_pid end,
}
LSO.__get = {
	weaponsComponent = function() return M.px(LWC, 'SoldierWeaponsComponent') end,
	isAlive = function() return M.snap ~= nil and M.snap.alive == true end,
	health = function() return M.snap and M.snap.hp or 0.0 end,
	pose = function() return M.snap and M.snap.pose or 0 end,
	typeInfo = function() return M.typeinfo('ClientSoldierEntity') end,
}
LWC.__get = {
	currentWeapon = function() return (M.snap and M.snap.w) and M.px(LW, 'SoldierWeapon') or nil end,
	isZooming = function() return M.snap ~= nil and (M.snap.zoom or 0) > 0 end,
}
LW.__get = {
	predictedWeaponFiring = function() return M.px(PRED, 'WeaponFiring') end,
	correctedWeaponFiring = function() return M.px(CORR, 'WeaponFiring') end,
	aimingSimulation = function() return M.px(LAIM, 'AimingSimulation') end,
	name = function() return M.snap and M.snap.w and M.snap.w.name or nil end,
	primaryAmmo = function() return M.snap and M.snap.w and M.snap.w.ammo or 0 end,
	weaponFiring = function() return M.px(PRED, 'WeaponFiring') end,
	typeInfo = function() return M.typeinfo('ClientSoldierWeapon') end,
}

local function apply_snap(s)
	M.snap = s
	if s.pid then M.local_pid = s.pid end
	if s.w and s.w.gs then
		local g = s.w.gs
		for _, wf in ipairs({ PRED, CORR }) do
			local o = wf.gs
			o.dispersionAngle, o.minDispersionAngle, o.fireShot, o.isFiring = g.da, g.dmin, g.fs, g.fir
			o.timeSinceLastShot, o.randomAngle, o.randomRadius, o.seed, o.initialFireShot = g.tsls, g.ra, g.rr, g.seed, g.ifs
			o.dev_rec.pitch, o.dev_rec.yaw = g.rp, g.ry
		end
	end
	if s.aim then
		local d = s.aim
		LAIM.yaw = math.atan(d[1], d[3])
		LAIM.pitch = math.asin(math.max(-1, math.min(1, d[2])))
	end
	LAIM.zoomLevel = s.zoom or 0
	LAIM.fovMultiplier = (M.VFOV[s.zoom or 0] or 55) / 55
end

-- Camera and projection ---------------------------------------------------------------------------------------------
local function cam_basis()
	local s = M.snap
	if s == nil or not s.alive or s.eye == nil then return nil end
	local f = A.norm(s.aim)
	local r, u = A.basis(f)        -- r = f x up (right), u = up
	local l = A.scale(r, -1)
	return s.eye, f, l, u
end

M.IMPL.ClientUtils = {
	GetCameraTransform = function()
		local e, f, l, u = cam_basis()
		if e == nil then return nil end
		-- forward sign [mock]: the camera forward points along the view (the mod tests the sign with WorldToScreen)
		local sg = M.laws.cam_sign or 1
		return M.lt(M.a3v(l), M.a3v(u), M.a3v(A.scale(f, sg)), M.a3v(e))
	end,
	GetWindowSize = function() return M.vec2(M.WIN[1], M.WIN[2]) end,
	WorldToScreen = function(_, p)
		if not M.is_vec3(p) then error('mock: WorldToScreen needs a Vec3', 2) end
		local e, f, l, u = cam_basis()
		if e == nil then return nil end
		local d = A.sub(M.v3a(p), e)
		local z = A.dot(d, f)
		if z <= 1e-6 then return nil end
		local vf = math.rad(M.VFOV[(M.snap and M.snap.zoom) or 0] or 55)
		local fy = (M.WIN[2] / 2) / math.tan(vf / 2)
		local x = M.WIN[1] / 2 - fy * A.dot(d, l) / z
		local y = M.WIN[2] / 2 - fy * A.dot(d, u) / z
		return M.vec2(x, y)
	end,
}

M.IMPL.PlayerManager = {
	GetLocalPlayer = function() return M.local_pid >= 0 and M.px(LP, 'Player') or nil end,
	GetPlayerById = function(_, id) if id == M.local_pid then return M.px(LP, 'Player') end return nil end,
}

M.IMPL.Console = {
	Register = function(_, name, desc, fn)
		if type(name) ~= 'string' or type(fn) ~= 'function' then error('mock: Console:Register(name, desc, fn)', 2) end
		local o = { __cls = 'ConsoleCommand', __owner = 'none', name = name, groupName = 'mharness', fn = fn }
		M.console[name] = o
		return M.px(o, 'ConsoleCommand')
	end,
}

M.IMPL.Events = {
	Subscribe = function(_, name, a, b) return M.subscribe(name, a, b) end,
	Unsubscribe = function(_, name) for _, s in ipairs(M.subs[name] or {}) do s.gone = true end end,
}
M.IMPL.Hooks = { Install = function(_, name, prio, a, b) return M.hook_install(name, prio, a, b) end }
M.IMPL.NetEvents = {
	Subscribe = function(_, name, a, b) return M.net_subscribe(name, a, b) end,
	Send = function(_, name, ...) M.net_send(name, 0, ...) end,
}
M.IMPL.SharedUtils = {
	GetTimeMS = function() return M.ms() + (M.laws.client_ms_offset or 0) end,
	GetTime = function() return M.t end,
	GetTickrate = function() return M.hz + 0.0 end,
	IsServerModule = function() return false end,
	IsClientModule = function() return true end,
}

local function lib(name) return M.px({ __cls = name, __owner = 'none' }, name) end

function M.client_globals()
	local g = {}
	for _, n in ipairs({ 'Events', 'Hooks', 'NetEvents', 'PlayerManager', 'SharedUtils', 'ClientUtils', 'Console' }) do
		g[n] = lib(n)
	end
	g.Vec3 = function(x, y, z) return M.vec3(x, y, z) end
	g.LinearTransform = function(...) return M.lt(...) end
	g.json = M.json
	g.class = function() error('mock: class() is not used by the mod', 2) end
	for name, tbl in pairs(M.enum_tables()) do g[name] = tbl end
	return g
end

function M.start_mod(sources, order, vm)
	M.sources = sources
	local errs = M.load_mod(sources, 'client', order, vm, M.client_globals())
	for _, e in ipairs(errs) do M.errors[#M.errors + 1] = 'load: ' .. e end
	return #errs
end

-- console(text): the user typed 'mh <args>' (or 'mharness.mh <args>') in the client console
function M.console_cmd(text)
	local name, rest = text:match('^(%S+)%s*(.*)$')
	if name == nil then return 'empty' end
	local short = name:match('%.([%w_]+)$') or name
	local c = M.console[short]
	if c == nil then return 'unknown command ' .. name end
	local args = {}
	for tok in rest:gmatch('%S+') do args[#args + 1] = tok end
	local ok, r = M.handler_call('console ' .. name, c.fn, args)
	return ok and tostring(r) or 'error'
end

function M.level_loaded(name, mode)
	M.dispatch('Level:Loaded', name, mode)
end

-- frames(snaps, events): one client frame per server tick snapshot; server NetEvents are delivered at the first frame
-- after the tick they were sent in (latency one tick).
function M.frames(snaps, events)
	local ei = 1
	local nev = #events
	for _, s in ipairs(snaps) do
		while ei <= nev and events[ei].k < s.k do
			local ev = events[ei]
			M.net_deliver(ev.name, nil, ev.args)
			ei = ei + 1
		end
		M.k = s.k
		M.t = s.k * M.dt
		apply_snap(s)
		M.dispatch('Engine:Update', M.dt, M.dt)
		if s.alive and s.w then
			M.dispatch('GunSway:Update', M.px(PRED.gs, 'GunSway'), M.px(LW, 'SoldierWeapon'), M.px(PRED, 'WeaponFiring'), M.dt)
			M.dispatch('GunSway:Update', M.px(CORR.gs, 'GunSway'), M.px(LW, 'SoldierWeapon'), M.px(CORR, 'WeaponFiring'), M.dt)
		end
	end
	while ei <= nev do
		local ev = events[ei]
		M.net_deliver(ev.name, nil, ev.args)
		ei = ei + 1
	end
end

function M.take_errors()
	local out = M.errors
	M.errors = {}
	return out
end

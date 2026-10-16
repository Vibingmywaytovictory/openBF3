-- Mock weapons (DESIGN.md 13.2, 13.3): fire timing from rpm, fire modes, magazine and reload with WeaponState
-- transitions, GunSway (spread min/max/increase/decay per stance and zoom, moving minimum, recoil with first-shot
-- multiplier and re-arm rule, recovery law), the random polar sample, projectiles with drop, EntityFactory:Create,
-- BulletEntity:Collision and Soldier:Damage hooks, falloff, zones, explosives, suppression.
-- The numbers are VU-like test values (data/weapon_constants.json where it has them) [mock]; tests must not depend on
-- the invented ones.
mock = mock or {}
local M = mock
local A = M.A
local W = M.W
local S = M.S

local WPN = {}
M.WPN = WPN

local function sp(min, max, inc) return { min = min, max = max, inc = inc } end
local RIFLE_SPREAD = {
	[0] = { hip = sp(2.5, 7, 0.1), ads = sp(0.2, 3, 0.1) },
	[1] = { hip = sp(2.0, 6, 0.1), ads = sp(0.15, 2.5, 0.1) },
	[2] = { hip = sp(1.5, 5, 0.1), ads = sp(0.1, 2, 0.1) },
}

WPN.DEF = {
	M16A3 = { kind = 'bullet', slot = 0, rpm = 800, mag = 31, reserve = 120, v0 = 650, g = 15, modes = { 0, 3, 2 },
		spread = RIFLE_SPREAD, move = { hip = 1.0, ads = 0.3 }, recoil = { up = 0.26, hmin = -0.4, hmax = 0.1, fsrm = 2.5 },
		falloff = { 25, 18.4, 8, 50 }, reload = 2.5 },
	AK74M = { kind = 'bullet', slot = 0, rpm = 650, mag = 31, reserve = 120, v0 = 600, g = 15, modes = { 0, 2 },
		spread = RIFLE_SPREAD, move = { hip = 1.0, ads = 0.3 }, recoil = { up = 0.28, hmin = -0.3, hmax = 0.2, fsrm = 1.5 },
		falloff = { 25, 18.4, 8, 50 }, reload = 2.5 },
	M416 = { kind = 'bullet', slot = 0, rpm = 750, mag = 31, reserve = 120, v0 = 600, g = 15, modes = { 0, 3, 2 },
		spread = RIFLE_SPREAD, move = { hip = 1.0, ads = 0.3 }, recoil = { up = 0.26, hmin = -0.3, hmax = 0.1, fsrm = 2.0 },
		falloff = { 25, 18.4, 8, 50 }, reload = 2.5 },
	M249 = { kind = 'bullet', slot = 0, rpm = 800, mag = 100, reserve = 200, v0 = 620, g = 15, modes = { 2 },
		spread = { [0] = { hip = sp(5, 7, 0.1), ads = sp(0.5, 3, 0.1) }, [1] = { hip = sp(4, 7, 0.1), ads = sp(0.4, 3, 0.1) },
			[2] = { hip = sp(3, 7, 0.1), ads = sp(0.3, 3, 0.1) } }, move = { hip = 1.0, ads = 0.3 },
		recoil = { up = 0.4, hmin = -0.4, hmax = 0.4, fsrm = 2.2 }, falloff = { 25, 18.4, 8, 50 }, reload = 5.0 },
	M98B = { kind = 'bullet', slot = 0, rpm = 46.2, mag = 6, reserve = 24, v0 = 650, g = 9.81, modes = { 1 },
		spread = { [0] = { hip = sp(3, 6, 1), ads = sp(0, 0, 0) }, [1] = { hip = sp(2.5, 6, 1), ads = sp(0, 0, 0) },
			[2] = { hip = sp(2, 6, 1), ads = sp(0, 0, 0) } }, move = { hip = 1.0, ads = 0.0 },
		recoil = { up = 3.0, hmin = -0.5, hmax = 0.5, fsrm = 1.0 }, falloff = { 95, 59, 15, 150 }, reload = 3.5, bolt = 1.3 },
	M9 = { kind = 'bullet', slot = 1, rpm = 400, mag = 16, reserve = 48, v0 = 380, g = 15, modes = { 0 },
		spread = { [0] = { hip = sp(1.5, 5, 0.3), ads = sp(0.4, 3, 0.3) }, [1] = { hip = sp(1.2, 5, 0.3), ads = sp(0.3, 3, 0.3) },
			[2] = { hip = sp(1.0, 5, 0.3), ads = sp(0.2, 3, 0.3) } }, move = { hip = 0.5, ads = 0.2 },
		recoil = { up = 0.5, hmin = -0.3, hmax = 0.3, fsrm = 1.5 }, falloff = { 25, 13.75, 12, 50 }, reload = 2.0 },
	['870MCS'] = { kind = 'bullet', slot = 0, rpm = 70, mag = 7, reserve = 28, v0 = 300, g = 9.81, modes = { 1 },
		spread = { [0] = { hip = sp(1.5, 3, 0), ads = sp(1.5, 3, 0) }, [1] = { hip = sp(1.5, 3, 0), ads = sp(1.5, 3, 0) },
			[2] = { hip = sp(1.5, 3, 0), ads = sp(1.5, 3, 0) } }, move = { hip = 0, ads = 0 },
		recoil = { up = 2.0, hmin = -0.5, hmax = 0.5, fsrm = 1.0 }, falloff = { 100, 25, 5, 30 }, reload = 4.0, bolt = 0.8 },
	RPG7 = { kind = 'missile', slot = 2, rpm = 30, mag = 1, reserve = 4, v0 = 150, g = 0, modes = { 0 }, direct = 'rpg',
		blast = 112, inner = 0.35, radius = 4, reload = 3.0 },
	SMAW = { kind = 'missile', slot = 2, rpm = 30, mag = 1, reserve = 4, v0 = 150, g = 0, modes = { 0 }, direct = 'rpg',
		blast = 112, inner = 0.35, radius = 4, reload = 3.0 },
	M320HE = { kind = 'grenade', slot = 2, rpm = 30, mag = 1, reserve = 8, v0 = 75, g = 0, modes = { 0 }, direct = 100,
		blast = 95, inner = 0.5, radius = 5, reload = 2.5 },
	M67 = { kind = 'grenade', slot = 6, rpm = 30, mag = 1, reserve = 2, v0 = 15, g = 0, modes = { 0 }, blast = 100,
		inner = 1.0, radius = 7, fuse = 3.5, thrown = true, reload = 1.0 },
}
for key, d in pairs(WPN.DEF) do d.key = key end

-- asset names (lower case) -> weapon key; plus kits, blueprint and appearance names (ResourceManager)
WPN.ASSETS = {
	['weapons/m16a4/u_m16a4'] = 'M16A3', ['weapons/ak74m/u_ak74m'] = 'AK74M', ['weapons/m416/u_m416'] = 'M416',
	['weapons/m249/u_m249'] = 'M249', ['weapons/model98b/u_m98b'] = 'M98B', ['weapons/m9/u_m9'] = 'M9',
	['weapons/remington870/u_870'] = '870MCS', ['weapons/rpg7/u_rpg7'] = 'RPG7', ['weapons/smaw/u_smaw'] = 'SMAW',
	['weapons/gadgets/m320/u_m320_he'] = 'M320HE', ['weapons/m67/u_m67'] = 'M67',
}
WPN.CANON = {
	M16A3 = 'Weapons/M16A4/U_M16A4', AK74M = 'Weapons/AK74M/U_AK74M', M416 = 'Weapons/M416/U_M416',
	M249 = 'Weapons/M249/U_M249', M98B = 'Weapons/Model98B/U_M98B', M9 = 'Weapons/M9/U_M9',
	['870MCS'] = 'Weapons/Remington870/U_870', RPG7 = 'Weapons/RPG7/U_RPG7', SMAW = 'Weapons/SMAW/U_SMAW',
	M320HE = 'Weapons/Gadgets/M320/U_M320_HE', M67 = 'Weapons/M67/U_M67',
}
WPN.OTHER_ASSETS = {
	'Characters/Soldiers/MpSoldier', 'Gameplay/Kits/USAssault', 'Gameplay/Kits/RUAssault',
	'Persistence/Unlocks/Soldiers/Visual/MP/Us/MP_US_Assault_Appearance01',
	'Persistence/Unlocks/Soldiers/Visual/MP/RU/MP_RU_Assault_Appearance01',
}

local DATA = {}
local function data(dtype, parent)
	local d = DATA[dtype]
	if d then return d end
	d = { __cls = 'DataContainer', __owner = 'none', __dtype = dtype, __dparent = parent }
	d.__get = { typeInfo = function() return M.typeinfo(dtype) end }
	DATA[dtype] = d
	return d
end
WPN.data = data

-- Equip --------------------------------------------------------------------------------------------------------------
local WEAPON_GET = {
	weaponFiring = function(o) return M.px(o.wf, 'WeaponFiring') end,
	typeInfo = function() return M.typeinfo('SoldierWeapon') end,
	transform = function(o) return M.lt() end,
	isPendingDestruction = function() return false end,
	data = function() return nil end,
}
local WF_GET = { gunSway = function(o) return M.px(o.gs, 'GunSway') end }
local GS_GET = {
	currentRecoilDeviation = function(o) return M.px(o.dev_rec, 'GunSwayDeviation') end,
	currentDispersionDeviation = function(o) return M.px(o.dev_disp, 'GunSwayDeviation') end,
	currentLagDeviation = function(o) return M.px(o.dev_lag, 'GunSwayDeviation') end,
	cameraRecoilTransform = function() return M.lt() end,
}

local function dev() return { __cls = 'GunSwayDeviation', __owner = 'none', pitch = 0.0, yaw = 0.0, roll = 0.0, transY = 0.0 } end

local function initial_mode(d)
	local want = ({ auto = 2, single = 0, burst = 3, stuck = 0 })[M.laws.fire_mode or 'auto'] or 2
	for i, m in ipairs(d.modes) do if m == want then return i end end
	return 1
end

function WPN.new_weapon(key, owner, aname)
	local d = WPN.DEF[key]
	local mi = initial_mode(d)
	local gs = { __cls = 'GunSway', __owner = 'none', dispersionAngle = 0.0, minDispersionAngle = 0.0,
		timeSinceLastShot = 10.0, crossHairDispersionFactor = 1.0, currentGameplayDeviationScaleFactor = 1.0,
		currentVisualDeviationScaleFactor = 1.0, suppressionMinDispersionAngleFactor = 1.0, randomAngle = 0.0,
		randomRadius = 0.0, jumpingTimer = 0.0, seed = 0, initialFireShot = 0, fireShot = false, isFiring = false,
		isJumping = false, isMoving = false, isSupportedShooting = false, dev_rec = dev(), dev_disp = dev(), dev_lag = dev(),
		__get = GS_GET }
	M.next_iid = M.next_iid + 1
	local wf = { __cls = 'WeaponFiring', __owner = 'none', gs = gs, weaponState = 0, lastWeaponState = 3, nextWeaponState = 3,
		timeToWait = 0.0, recoilTimer = 0.0, recoilAngleX = 0.0, recoilAngleY = 0.0, recoilAngleZ = 0.0, recoilFovAngle = 0.0,
		primaryAmmoToFill = 0, instanceId = M.next_iid, __get = WF_GET }
	local w = { __cls = 'SoldierWeapon', __owner = owner, def = d, name = aname or WPN.CANON[key], primaryAmmo = d.mag,
		secondaryAmmo = d.reserve, fireLogic = d.modes[mi], mode_i = mi, wf = wf, slot = d.slot, __get = WEAPON_GET,
		st = { p = 0.0, y = 0.0, da = nil, tsls = 10.0, next_t = -1e9, last_t = -1e9, armed = true, released = true,
			pending = false, burst = 0, reload_t = 0, post_t = 0, deploy_t = 0.6, bolt_t = 0, last_kick = 0, hz = 0,
			seed = 0, ifs = 0, shots = 0 } }
	M.next_iid = M.next_iid + 1
	w.instanceId = M.next_iid
	w.uniqueId = W.uid()
	return w
end

local WC_GET = {
	currentWeapon = function(o)
		local w = o.w[o.slot]
		return w and M.px(w, 'SoldierWeapon') or nil
	end,
	currentWeaponSlot = function(o) return o.slot end,
	isZooming = function(o) return o.so.zoom == true end,
	weapons = function(o)
		local out = {}
		for s = 0, 7 do if o.w[s] then out[#out + 1] = M.px(o.w[s], 'SoldierWeapon') end end
		return out
	end,
	typeInfo = function() return M.typeinfo('SoldierWeaponsComponent') end,
	data = function() return nil end,
}

function WPN.equip_soldier(so, pl)
	local wc = { __cls = 'SoldierWeaponsComponent', __owner = 'none', so = so, w = {}, slot = 0, __get = WC_GET }
	for slot, asset in pairs(pl.sel or {}) do
		if asset.key and WPN.DEF[asset.key] then wc.w[slot] = WPN.new_weapon(asset.key, pl.__owner, asset.aname) end
	end
	if wc.w[0] == nil then
		local key = pl.teamId == 2 and 'AK74M' or 'M16A3'
		wc.w[0] = WPN.new_weapon(key, pl.__owner)
	end
	so.wc = wc
	M.next_iid = M.next_iid + 1
	wc.instanceId = M.next_iid
end

-- swap(so, key): the scripted user's loadout change (the deploy-menu equivalent) puts key into its slot and selects it.
function WPN.swap(so, key)
	local d = WPN.DEF[key]
	so.wc.w[d.slot] = WPN.new_weapon(key, so.__owner)
	so.wc.slot = d.slot
end

function WPN.current(so)
	if so.wc == nil then return nil end
	return so.wc.w[so.wc.slot]
end

-- Spread / recoil dynamics ---------------------------------------------------------------------------------------------------
local function spread_of(w, so)
	local t = w.def.spread
	if t == nil then return { min = 0, max = 0, inc = 0 }, 0 end
	local zs = t[so.pose] or t[0]
	local z = so.zoom and zs.ads or zs.hip
	local add = 0
	if so.moving and w.def.move then add = so.zoom and w.def.move.ads or w.def.move.hip end
	return z, add
end

-- recovery(w, dt, firing, frac): frac = this substep's share of the tick (coupled=recovery applies the 30 Hz amount
-- per tick, whatever the tick rate: the T1 tick-coupled variant).
local function recovery(w, dt, firing, frac)
	local st = w.st
	local rate = M.laws.rec_rate or 8
	local scale = firing and 0.1 or 1.0
	local coupled = M.laws.coupled == 'recovery'
	if M.laws.recovery == 'exp' then
		local f
		if coupled then f = math.exp(-rate * scale / 30 * frac) else f = math.exp(-rate * scale * dt) end
		st.p = st.p * f
		st.y = st.y * f
		if math.abs(st.p) < 1e-6 then st.p = 0.0 end
		if math.abs(st.y) < 1e-6 then st.y = 0.0 end
	else
		local m = rate * scale * dt
		if coupled then m = rate * scale / 30 * frac end
		local function toward0(x)
			if x > m then return x - m elseif x < -m then return x + m end
			return 0.0
		end
		st.p = toward0(st.p)
		st.y = toward0(st.y)
	end
end

local function decay(w, so, dt, frac)
	local st = w.st
	local z, add = spread_of(w, so)
	local dmin = z.min + add
	local rate = M.laws.decay or 15
	if st.da == nil then st.da = dmin end
	local interval = 60 / w.def.rpm
	local over = st.tsls - interval
	if over > 0 then
		local amt
		if M.laws.coupled == 'decay' then amt = rate / 30 * frac * math.min(1, over / dt)
		else amt = rate * math.min(dt, over) end
		if st.da > dmin then st.da = math.max(dmin, st.da - amt) end
	end
	if st.da < dmin then
		if M.laws.min_rise == 'ramp' then st.da = math.min(dmin, st.da + rate * dt) else st.da = dmin end
	end
	return dmin, z
end

local function armed_now(w, so, dmin)
	local st = w.st
	local rule = M.laws.rearm or 'release'
	if st.shots == 0 then return true end
	if rule == 'release' then return st.released
	elseif rule == 'recovered' then return math.abs(st.p) <= 0.05 * math.max(st.last_kick, 1e-9)
	elseif rule == 'spreadmin' then return st.da <= dmin + 1e-9 end
	local T = tonumber(tostring(rule):match('^timer:([%d.]+)$') or '')
	if T then return st.tsls >= T - 1e-9 end
	return st.released
end

-- Fire -------------------------------------------------------------------------------------------------------------------
local function ra_out(phi, r, da)
	local u = M.laws.ra_units or 'deg'
	if u == 'rad' then return phi, math.rad(r) end
	if u == 'norm' then return phi / (2 * math.pi), (da > 0) and (r / da) or 0 end
	return math.deg(phi), r
end

function WPN.muzzle(so)
	local e = S.eye(so)
	return A.add(e, A.scale(so.aim, 0.25))
end

local PROJ_GET = {
	transform = function(o)
		if o.destroyed then error('mock: entity destroyed', 2) end
		return M.lt(M.vec3(1, 0, 0), M.vec3(0, 1, 0), M.a3v(A.norm(o.vel)), M.a3v(o.p))
	end,
	typeInfo = function(o) return M.typeinfo(o.__etype) end,
	isPendingDestruction = function(o) return o.destroyed == true end,
	data = function() return nil end,
}

WPN.proj = {}

local DTYPE = { bullet = { 'BulletEntityData', 'ServerBulletEntity' }, missile = { 'MissileEntityData', 'ServerMissileEntity' },
	grenade = { 'GrenadeEntityData', 'ServerGrenadeEntity' } }

-- spawn_projectile(so, w, dir, origin, extra): through the EntityFactory:Create hook (ef_bullets=false: bullets bypass).
function WPN.spawn_projectile(so, w, dir, origin, extra)
	local d = w.def
	local dt = DTYPE[d.kind]
	local function create()
		local e = { __cls = 'GameEntity', __etype = dt[2], __owner = 'none', p = A.copy(origin),
			vel = A.scale(dir, d.v0), g = d.g or 0, so = so, pl = so.pl, def = d, w = w, origin = A.copy(origin),
			born = M.t, factor = 1.0, __get = PROJ_GET, hitset = {}, suppressed = {} }
		for k2, v in pairs(extra or {}) do e[k2] = v end
		W.add_ent(e)
		WPN.proj[#WPN.proj + 1] = e
		return M.px(e, 'Entity')
	end
	local fwd = A.norm(dir)
	local up = { 0, 1, 0 }
	local left = A.norm(A.cross(up, fwd))
	if A.len(left) < 0.5 then left = { 1, 0, 0 } end
	local up2 = A.cross(fwd, left)
	local tr = M.lt(M.a3v(left), M.a3v(up2), M.a3v(fwd), M.a3v(origin))
	if d.kind == 'bullet' and M.laws.ef_bullets == false then return create() end
	return M.run_hook('EntityFactory:Create', function() return create() end,
		M.px(data(dt[1], 'MeshProjectileEntityData'), 'DataContainer'), tr)
end

local function fire(so, w, dmin, z)
	local st, d, gs = w.st, w.def, w.wf.gs
	local rng = M.rng_world
	local armed = armed_now(w, so, dmin)
	-- direction: aim + recoil before this shot's kick + dispersion sample
	local dir = A.deviate(so.aim, st.y, st.p)
	local da = st.da or dmin
	local phi = 2 * math.pi * rng.u()
	local u = rng.u()
	local r = (M.laws.disc == 'radius') and (da * u) or (da * math.sqrt(u))
	if (M.out_left or 0) > 0 then
		M.out_left = M.out_left - 1
		r = (M.out_f or 1.2) * da
	end
	dir = A.deviate(dir, r * math.cos(phi), r * math.sin(phi))
	if (M.laws.aim_bias or 0) ~= 0 and so.__owner == 'bot' then
		local b = math.rad(M.laws.aim_bias)
		local c, s = math.cos(b), math.sin(b)
		dir = { dir[1] * c + dir[3] * s, dir[2], -dir[1] * s + dir[3] * c }
	end
	gs.randomAngle, gs.randomRadius = ra_out(phi, r, da)
	gs.dev_disp.pitch, gs.dev_disp.yaw = r * math.sin(phi), r * math.cos(phi)
	st.seed = st.seed + 1
	gs.seed = st.seed
	local origin = WPN.muzzle(so)
	local ent = WPN.spawn_projectile(so, w, dir, origin, nil)
	-- kick
	local rc = d.recoil
	if rc then
		local kick = rc.up * (armed and rc.fsrm or 1.0)
		local hz = M.laws.hcorr or 0
		local nz = rng.n()
		st.hz = hz * st.hz + math.sqrt(math.max(0, 1 - hz * hz)) * nz
		local uu = 0.5 + 0.5 * math.max(-1, math.min(1, st.hz / 2))
		local h = rc.hmin + (rc.hmax - rc.hmin) * uu
		st.p = st.p + kick
		st.y = st.y + h
		st.last_kick = kick
	end
	-- spread increase
	if z then st.da = math.min(math.max(z.max, dmin), (st.da or dmin) + z.inc) end
	st.tsls = 0.0
	st.last_t = M.t
	st.shots = st.shots + 1
	st.released = false
	st.ifs = st.ifs + 1
	w.primaryAmmo = w.primaryAmmo - 1
	gs.fireShot = true
	if d.bolt then st.bolt_t = d.bolt end
	return ent
end

local function set_state(w, s)
	local wf = w.wf
	if wf.weaponState ~= s then
		wf.lastWeaponState = wf.weaponState
		wf.weaponState = s
	end
end

-- update(so, dt, nsub): input edges, timers, spread/recoil, firing and GunSway:Update per substep.
function WPN.update(so, dt, nsub)
	local w = WPN.current(so)
	if w == nil then return end
	local c = so.ctl or {}
	local st, d, wf, gs = w.st, w.def, w.wf, w.wf.gs
	local trig = c.fire == true
	-- weapon select
	if c.sel ~= nil and S.edge(so, 'sel' .. c.sel, true) then
		if so.wc.w[c.sel] and so.wc.slot ~= c.sel then
			so.wc.slot = c.sel
			w = so.wc.w[c.sel]
			w.st.deploy_t = 0.6
			st, d, wf, gs = w.st, w.def, w.wf, w.wf.gs
		end
	end
	for n = 0, 4 do if c.sel ~= n then S.edge(so, 'sel' .. n, false) end end
	-- fire mode cycle
	if S.edge(so, 'cyc', c.cyc) and M.laws.fire_mode ~= 'stuck' and #d.modes > 1 then
		w.mode_i = w.mode_i % #d.modes + 1
		w.fireLogic = d.modes[w.mode_i]
	end
	-- reload
	if S.edge(so, 'reload', c.reload) and st.reload_t <= 0 and w.primaryAmmo < d.mag and w.secondaryAmmo > 0 then
		st.reload_t = d.reload or 2.5
		M.dispatch('Player:Reload', M.px(so.pl, 'Player'), w.name, M.a3v(so.p))
	end
	if not trig then
		st.released = true
		st.pending = false
		st.ifs = 0
	elseif S.edge(so, 'trig', true) then
		st.pending = true
		if w.fireLogic == 3 then st.burst = 3 end
	end
	if not trig then S.edge(so, 'trig', false) end
	-- timers
	if st.deploy_t > 0 then st.deploy_t = st.deploy_t - dt end
	if st.bolt_t > 0 then st.bolt_t = st.bolt_t - dt end
	if st.post_t > 0 then st.post_t = st.post_t - dt end
	if st.reload_t > 0 then
		st.reload_t = st.reload_t - dt
		if st.reload_t <= 0 then
			local need = d.mag - w.primaryAmmo
			local take = math.min(need, w.secondaryAmmo)
			w.primaryAmmo = w.primaryAmmo + take
			w.secondaryAmmo = w.secondaryAmmo - take
			st.post_t = 0.3
		end
	end
	local can = st.deploy_t <= 0 and st.reload_t <= 0 and st.bolt_t <= 0 and w.primaryAmmo > 0 and so.alive
	local interval = 60 / d.rpm
	local want = false
	if can then
		local fl = w.fireLogic
		if fl == 2 then want = trig
		elseif fl == 3 then want = st.burst > 0 or st.pending
		else want = st.pending end
	end
	local shoot = false
	if want then
		if st.next_t < M.t - dt - 1e-9 then st.next_t = M.t - 1e-9 end
		if M.t + 1e-9 >= st.next_t then shoot = true end
	end
	so.wc.firing = trig and can
	gs.fireShot = false
	local firing = trig and can
	local sdt = dt / nsub
	local ent = nil
	for i = 1, nsub do
		st.tsls = st.tsls + sdt
		recovery(w, sdt, firing, 1 / nsub)
		local dmin, z = decay(w, so, sdt, 1 / nsub)
		if i == nsub then
			if shoot then
				ent = fire(so, w, dmin, z)
				st.next_t = math.max(st.next_t, M.t - dt) + interval
				st.pending = false
				if w.fireLogic == 3 then st.burst = math.max(0, st.burst - 1) end
			end
		end
		-- GunSway fields
		gs.dispersionAngle = st.da or dmin
		gs.minDispersionAngle = dmin
		gs.dev_rec.pitch, gs.dev_rec.yaw = st.p, st.y
		gs.timeSinceLastShot = st.tsls
		gs.isFiring = firing
		gs.isMoving = so.moving == true
		gs.isJumping = so.jumping == true
		gs.jumpingTimer = so.jumping and (M.t - (so.jump_t or M.t)) or 0.0
		gs.initialFireShot = st.ifs
		gs.suppressionMinDispersionAngleFactor = (M.t < (so.supp_until or -1)) and 1.5 or 1.0
		if i < nsub then gs.fireShot = false end
		if M.laws.gs_server ~= false then
			M.dispatch('GunSway:Update', M.px(gs, 'GunSway'), M.px(w, 'SoldierWeapon'), M.px(wf, 'WeaponFiring'), sdt)
		end
	end
	-- weapon state
	if st.reload_t > 0 then set_state(w, 11)
	elseif st.post_t > 0 then set_state(w, 12)
	elseif st.deploy_t > 0 then set_state(w, 0)
	elseif st.bolt_t > 0 then set_state(w, 8)
	elseif firing and shoot then set_state(w, w.fireLogic == 2 and 9 or 6)
	elseif not trig then set_state(w, 3) end
	wf.nextWeaponState = 3
	wf.timeToWait = math.max(0, st.next_t - M.t)
	wf.recoilTimer = st.tsls
	wf.recoilAngleX, wf.recoilAngleY, wf.recoilAngleZ = st.p, st.y, 0.0
	return ent
end

-- Damage -----------------------------------------------------------------------------------------------------------------------
local INFO_GET = {}
local GIVER_GET = {}

-- damage(victim_so, d): d = {damage, pos, org, dir, bone, bullet, explosion, client, speed, giver_pl, w}.
function WPN.damage(so, d)
	if not so.alive then return end
	local info = { __cls = 'DamageInfo', __owner = 'none', damage = d.damage, position = M.a3v(d.pos), origin = M.a3v(d.org),
		direction = M.a3v(d.dir or { 0, 0, 1 }), boneIndex = d.bone or -1, isBulletDamage = d.bullet == true,
		isExplosionDamage = d.explosion == true, isClientDamage = d.client == true, collisionSpeed = d.speed or 0.0,
		distributeDamageOverTime = 0.0, __get = INFO_GET }
	local giver = nil
	if d.giver_pl then
		giver = { __cls = 'DamageGiverInfo', __owner = 'none', giver = M.px(d.giver_pl, 'Player'), weaponUnlock = nil,
			damageType = d.explosion and 2 or 1, weaponFiring = M.px(data('WeaponFiringData'), 'DataContainer'),
			__get = GIVER_GET }
	end
	local amount = d.damage
	local function orig()
		if not so.alive then return nil end
		if S.immune(so) then return nil end
		if M.laws.prot_threshold and amount < 5 then return nil end
		so.hp = so.hp - amount
		so.last_dmg_t = M.t
		if so.hp <= 0 then
			so.hp = 0
			S.die(so, d.giver_pl, 'damage', { head = d.bone == 1 })
		end
		return nil
	end
	M.run_hook('Soldier:Damage', orig, M.px(so, 'SoldierEntity'), M.px(info, 'DamageInfo'),
		giver and M.px(giver, 'DamageGiverInfo') or nil)
end

function WPN.falloff(d, dist)
	local f = d.falloff
	if f == nil then return 100 end
	local d0, d1 = f[3], f[4]
	if d.key == 'M98B' then d1 = M.laws.m98b_end or 150 end
	if dist <= d0 then return f[1] end
	if dist >= d1 then return f[2] end
	local s = (dist - d0) / (d1 - d0)
	if M.laws.falloff == 'smooth' then s = s * s * (3 - 2 * s) end
	return f[1] + (f[2] - f[1]) * s
end

local function blast_at(d, r)
	if r >= d.radius then return 0 end
	if r <= d.inner then return d.blast end
	local s = (r - d.inner) / (d.radius - d.inner)
	if M.laws.blast == 'quadratic' then return d.blast * (1 - s) * (1 - s) end
	return d.blast * (1 - s)
end

function WPN.explode(e, pos, direct_so)
	local d = e.def
	M.run_hook('EntityFactory:Create', function()
		local x = { __cls = 'GameEntity', __etype = 'ServerExplosionEntity', __owner = 'none', p = A.copy(pos),
			vel = { 0, 0, 1 }, __get = PROJ_GET }
		return M.px(x, 'Entity')
	end, M.px(data('ExplosionEntityData'), 'DataContainer'),
		M.lt(M.vec3(1, 0, 0), M.vec3(0, 1, 0), M.vec3(0, 0, 1), M.a3v(pos)))
	for _, so in ipairs(S.alive_soldiers()) do
		if so ~= direct_so then
			local cen = { so.p[1], so.p[2] + S.height(so) / 2, so.p[3] }
			local r = A.dist(cen, pos)
			local dmg = blast_at(d, r)
			if dmg > 0 then
				WPN.damage(so, { damage = dmg, pos = cen, org = pos, dir = A.norm(A.sub(cen, pos)), bone = -1, bullet = false,
					explosion = true, client = e.pl.__owner == 'human', speed = 0, giver_pl = e.pl })
			end
		end
	end
end

local function destroy(e)
	e.destroyed = true
	W.remove_ent(e)
end

local HIT_GET = {}

local function collision_hook(e, h, giver_pl)
	local rb = nil
	if h.so then rb = M.px(h.so, 'Entity') elseif h.ent then rb = M.px(h.ent, 'Entity') end
	local noise = M.laws.hit_noise or 0
	local pos = h.pos
	if noise > 0 then
		pos = { pos[1] + noise * M.rng_world.n(), pos[2] + noise * M.rng_world.n(), pos[3] + noise * M.rng_world.n() }
	end
	local hit = { __cls = 'RayCastHit', __owner = 'none', position = M.a3v(pos), normal = M.a3v(h.n),
		material = h.mat and M.px(h.mat, 'DataContainer') or nil, part = h.so and 0 or -1, bone = h.bone or -1,
		rigidBody = rb, __get = HIT_GET }
	local giver = { __cls = 'DamageGiverInfo', __owner = 'none', giver = M.px(giver_pl, 'Player'), weaponUnlock = nil,
		damageType = 1, weaponFiring = M.px(data('WeaponFiringData'), 'DataContainer'), __get = GIVER_GET }
	M.run_hook('BulletEntity:Collision', function() return true end, M.px(e, 'Entity'), M.px(hit, 'RayCastHit'),
		M.px(giver, 'DamageGiverInfo'))
end

-- step_projectiles(dt): move, collide, damage, suppress.
function WPN.step_projectiles(dt)
	local list = WPN.proj
	WPN.proj = {}
	local keep = {}
	for _, e in ipairs(list) do
		if not e.destroyed then
			local a = e.p
			local d = e.def
			local b
			if e.target then
				-- thrown grenade: straight to the target point in 1.2 s, then rests until the fuse
				local tt = math.min(1, (M.t - e.born) / 1.2)
				b = A.add(e.origin, A.scale(A.sub(e.target, e.origin), tt))
			else
				b = { a[1] + e.vel[1] * dt, a[2] + e.vel[2] * dt - 0.5 * e.g * dt * dt, a[3] + e.vel[3] * dt }
				e.vel[2] = e.vel[2] - e.g * dt
			end
			local chars = {}
			for _, so in ipairs(S.alive_soldiers()) do
				if so ~= e.so and not e.hitset[so] then chars[#chars + 1] = so end
			end
			local hits = (e.target == nil) and W.raycast(a, b, 4, chars) or {}
			local done = false
			for _, h in ipairs(hits) do
				if done then break end
				if h.so then
					local H = S.height(h.so)
					local bone, zname, mult = W.zone(H, h.yrel, h.b)
					h.bone = bone
					e.hitset[h.so] = true
					collision_hook(e, h, e.pl)
					if d.kind == 'bullet' then
						local bot_shooter = e.pl.__owner == 'bot'
						if not (bot_shooter and M.laws.bot_damage == false) then
							local dist = A.dist(h.pos, e.origin)
							local dmg = WPN.falloff(d, dist) * mult * e.factor
							WPN.damage(h.so, { damage = dmg, pos = h.pos, org = e.origin, dir = A.norm(e.vel), bone = bone,
								bullet = true, client = e.pl.__owner == 'human', speed = A.len(e.vel), giver_pl = e.pl, w = e.w })
						end
						if M.laws.pass_soldiers then
							e.factor = e.factor * 0.8
						else
							done = true
						end
					else
						local direct = d.direct == 'rpg' and (M.laws.rpg_direct or 225) or d.direct
						if direct then
							WPN.damage(h.so, { damage = direct, pos = h.pos, org = e.origin, dir = A.norm(e.vel), bone = bone,
								bullet = false, explosion = false, client = e.pl.__owner == 'human', speed = A.len(e.vel), giver_pl = e.pl })
						end
						WPN.explode(e, h.pos, h.so)
						done = true
					end
				else
					collision_hook(e, h, e.pl)
					if d.kind ~= 'bullet' then WPN.explode(e, h.pos, nil) end
					done = true
				end
			end
			if not done and d.kind == 'bullet' then
				-- suppression: enemies within 2.5 m of the segment that were not hit
				for _, so in ipairs(chars) do
					if so.pl.teamId ~= e.pl.teamId and not e.suppressed[so] then
						local c = { so.p[1], so.p[2] + 1.0, so.p[3] }
						local ab = A.sub(b, a)
						local l2 = A.dot(ab, ab)
						local s = l2 > 0 and math.max(0, math.min(1, A.dot(A.sub(c, a), ab) / l2)) or 0
						if A.dist(c, A.add(a, A.scale(ab, s))) < 2.5 then
							e.suppressed[so] = true
							S.suppress(so, e.pl)
						end
					end
				end
			end
			if e.target and not done and M.t - e.born >= (d.fuse or 3.5) then
				WPN.explode(e, b, nil)
				done = true
			end
			e.p = b
			if done or M.t - e.born > 4.0 then destroy(e) else keep[#keep + 1] = e end
		end
	end
	for _, e in ipairs(WPN.proj) do keep[#keep + 1] = e end
	WPN.proj = keep
end

-- throw / launch for the scripted user (W-C)
function WPN.throw(so, target)
	local w = WPN.current(so)
	if w == nil or w.def.kind == 'bullet' then return false end
	local origin = WPN.muzzle(so)
	if w.def.thrown then
		WPN.spawn_projectile(so, w, A.norm(A.sub(target, origin)), origin, { target = A.copy(target) })
	else
		WPN.spawn_projectile(so, w, A.norm(A.sub(target, origin)), origin, nil)
	end
	return true
end

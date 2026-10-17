-- Mock players and soldiers (DESIGN.md 13.2, 13.3): Player and SoldierEntity objects, bot EntryInput reading, motor
-- laws, poses, jump, gravity and fall damage, health, regeneration, spawn immunity, suppression, death.
mock = mock or {}
local M = mock
local A = M.A
local W = M.W

local S = {}
M.S = S

S.players = {}       -- list of Player raws (creation order)
S.next_pid = 0
S.EYE = { [0] = 1.6, [1] = 1.0, [2] = 0.4 }
S.SPEED = { [0] = 3.6, [1] = 2.4, [2] = 1.0 }
S.SPRINT = 6.4
S.JUMP_V = 4.2
S.LIN_ACC = 12.0      -- motor=linear (m/s^2)
S.GAIN30 = 0.35       -- motor=fraction: fraction of the remaining speed per 30 Hz tick
S.TRUE_CAL = { sx = -1, sz = 1, off = math.pi / 2, unit = 'rad', ps = 1 } -- hidden aim convention [mock]

local A_THR, A_STR, A_FIRE, A_ZOOM, A_JUMP, A_POSE, A_PRONE, A_RELOAD, A_SPRINT, A_CYC = 0, 1, 7, 11, 12, 13, 14, 15, 32, 34

-- Player ------------------------------------------------------------------------------------------------------------
local PLAYER_GET = {
	soldier = function(o)
		local so = o.so
		if so ~= nil and so.alive then return M.px(so, 'SoldierEntity') end
		return nil
	end,
	alive = function(o) return o.so ~= nil and o.so.alive == true end,
	hasSoldier = function(o) return o.so ~= nil and o.so.alive == true end,
}

function S.new_player(name, team, squad, owner)
	local id = S.next_pid
	S.next_pid = id + 1
	local o = { __cls = 'Player', __owner = owner, id = id, name = name, teamId = team, squadId = squad or 0,
		isAllowedToSpawn = true, sel = {}, __get = PLAYER_GET }
	if owner == 'bot' then o.__sid = math.tointeger(tonumber(tostring(name):match('^mh(%d+)$') or '')) end
	S.players[#S.players + 1] = o
	return o
end

function S.remove_player(o)
	for i, p in ipairs(S.players) do
		if p == o then
			table.remove(S.players, i)
			break
		end
	end
	o.gone = true
end

function S.by_id(id)
	for _, p in ipairs(S.players) do if p.id == id then return p end end
	return nil
end

function S.alive_soldiers()
	local out = {}
	for _, p in ipairs(S.players) do
		if p.so and p.so.alive then out[#out + 1] = p.so end
	end
	return out
end

-- Soldier -------------------------------------------------------------------------------------------------------------
local function body_lt(o)
	local f = o.body or { 0, 0, 1 }
	local up = { 0, 1, 0 }
	local left = A.cross(up, f)
	return M.lt(M.a3v(left), M.a3v(up), M.a3v(f), M.a3v(o.p))
end

local SOLDIER_GET = {
	worldTransform = body_lt,
	transform = body_lt,
	velocity = function(o) return M.a3v(o.v) end,
	health = function(o) return o.hp end,
	maxHealth = function() return 100.0 end,
	internalHealth = function(o) return o.hp end,
	isAlive = function(o) return o.alive == true end,
	isDead = function(o) return o.alive ~= true end,
	isManDown = function() return false end,
	isFiring = function(o) return o.wc ~= nil and o.wc.firing == true end,
	isReloading = function(o)
		local w = o.wc and o.wc.w[o.wc.slot]
		return w ~= nil and w.st.reload_t > 0
	end,
	pendingPose = function(o) return o.pose end,
	player = function(o) return M.px(o.pl, 'Player') end,
	weaponsComponent = function(o) return M.px(o.wc, 'SoldierWeaponsComponent') end,
	typeInfo = function() return M.typeinfo('ServerSoldierEntity') end,
	physicsEntityBase = function() return nil end,
	data = function() return nil end,
	isPendingDestruction = function() return false end,
}
S.SOLDIER_GET = SOLDIER_GET

function S.new_soldier(pl, tr)
	local o = { __cls = 'SoldierEntity', __etype = 'ServerSoldierEntity', __owner = pl.__owner, __sid = pl.__sid, pl = pl,
		p = { 0, 0, 0 }, v = { 0, 0, 0 }, aim = { 0, 0, 1 }, body = { 0, 0, 1 }, pose = 0, hp = 100.0, alive = false,
		prev = {}, last_dmg_t = -1e9, last_supp_t = -1e9, supp_until = -1e9, spawn_t = 0, zoom = false,
		ctl = nil, __get = SOLDIER_GET }
	if tr ~= nil then
		o.p = M.v3a(tr.trans)
		o.body = A.norm({ tr.forward.x, 0, tr.forward.z })
		o.aim = A.copy(o.body)
	end
	o.uniqueId = W.uid()
	M.next_iid = M.next_iid + 1
	o.instanceId = M.next_iid
	return o
end

function S.eye(so)
	return { so.p[1], so.p[2] + S.EYE[so.pose], so.p[3] }
end

function S.height(so)
	return W.HEIGHT[so.pose] or 1.85
end

-- spawn(pl, so, tr, pose): the soldier enters the world with the player's selected weapons.
function S.spawn(pl, so, tr, pose)
	if pl.so and pl.so.alive and pl.so ~= so then S.die(pl.so, nil, 'replaced') end
	so.p = M.v3a(tr.trans)
	so.p[2] = math.max(so.p[2], W.ground_y(so.p))
	local f = A.norm({ tr.forward.x, 0, tr.forward.z })
	if A.len(f) < 0.5 then f = { 0, 0, 1 } end
	so.body = f
	so.aim = A.copy(f)
	so.v = { 0, 0, 0 }
	so.pose = math.tointeger(pose) or 0
	so.hp = 100.0
	so.alive = true
	so.spawn_t = M.t
	so.last_dmg_t = -1e9
	so.last_supp_t = -1e9
	so.prev = {}
	so.zoom = false
	pl.so = so
	M.WPN.equip_soldier(so, pl)
	W.add_ent(so)
	M.C.on_spawn(pl)
	M.dispatch('Player:Respawn', M.px(pl, 'Player'))
end

-- die(so, inflictor_pl, why, info)
function S.die(so, inflictor, why, info)
	if not so.alive then return end
	so.alive = false
	so.hp = 0.0
	so.v = { 0, 0, 0 }
	W.remove_ent(so)
	M.C.on_death(so.pl, why)
	local wname = nil
	M.dispatch('Player:Killed', M.px(so.pl, 'Player'), inflictor and M.px(inflictor, 'Player') or nil,
		M.a3v(so.p), wname, false, info and info.head == true or false, false, info and info.px or nil)
	if so.pl.so == so then so.pl.so = nil end
end

function S.set_position(so, p)
	so.p = A.copy(p)
	so.v = { 0, 0, 0 }
end

-- Inputs ---------------------------------------------------------------------------------------------------------------
local function aim_dir(cal, yaw, pitch)
	if cal.unit == 'deg' then yaw, pitch = math.rad(yaw), math.rad(pitch) end
	local a = yaw - cal.off
	local p = pitch * cal.ps
	local cp = math.cos(p)
	return A.norm({ cal.sx * math.sin(a) * cp, math.sin(p), cal.sz * math.cos(a) * cp })
end
S.aim_dir = aim_dir

-- bot_ctl(so): the control state from the bot player's EntryInput (nil = no input this tick).
local function bot_ctl(so)
	local ip = so.pl.input
	if ip == nil then return nil end
	local r = M.raw(ip)
	if r == nil then return nil end
	if M.laws.input_dt == 'required' and not ((r.deltaTime or 0) > 0) then return nil end
	local lv = r.levels
	local c = { thr = lv[A_THR] or 0, str = lv[A_STR] or 0, fire = (lv[A_FIRE] or 0) >= 0.5,
		zoom = (r.zoomLevel or 0) >= 1 or (lv[A_ZOOM] or 0) >= 0.5, jump = (lv[A_JUMP] or 0) >= 0.5,
		chpose = (lv[A_POSE] or 0) >= 0.5, prone = (lv[A_PRONE] or 0) >= 0.5, reload = (lv[A_RELOAD] or 0) >= 0.5,
		sprint = (lv[A_SPRINT] or 0) >= 0.5, cyc = (lv[A_CYC] or 0) >= 0.5, sel = nil }
	for n = 1, 5 do
		if (lv[15 + n] or 0) >= 0.5 then c.sel = n - 1 end
	end
	if ((r.flags or 0) & 1) ~= 0 then
		c.aim = aim_dir(S.TRUE_CAL, r.authoritativeAimingYaw or 0, r.authoritativeAimingPitch or 0)
	end
	return c
end

local function edge(so, key, now)
	local was = so.prev[key] == true
	so.prev[key] = now == true
	return now == true and not was
end

-- Motor ------------------------------------------------------------------------------------------------------------------
local function motor_step(cur, want, dt)
	local law = M.laws.motor
	if law == 'instant' then return want end
	if law == 'linear' then
		local d = want - cur
		local m = S.LIN_ACC * dt
		if M.laws.coupled == 'accel' then m = S.LIN_ACC / 30 end
		if d > m then d = m elseif d < -m then d = -m end
		return cur + d
	end
	local g = S.GAIN30
	if M.laws.coupled ~= 'accel' then g = 1 - (1 - S.GAIN30) ^ (30 * dt) end
	return cur + (want - cur) * g
end

-- step(so, dt): one sim tick of one soldier (input, pose, motor, gravity, fall damage).
function S.step(so, dt)
	local c
	if so.__owner == 'bot' then c = bot_ctl(so) else c = so.uctl end
	so.ctl = c
	c = c or { thr = 0, str = 0 }
	if c.aim then
		so.aim = A.norm(c.aim)
		local h = A.norm({ so.aim[1], 0, so.aim[3] })
		if A.len(h) > 0.5 then so.body = h end
	end
	so.zoom = c.zoom == true
	if edge(so, 'chpose', c.chpose) then so.pose = (so.pose == 1) and 0 or 1 end
	if edge(so, 'prone', c.prone) then so.pose = (so.pose == 2) and 0 or 2 end
	if c.setpose then so.pose = c.setpose end
	-- horizontal target velocity
	local f = so.body
	local right = { f[3], 0, -f[1] }
	local base = S.SPEED[so.pose] or 3.6
	local sprint = c.sprint and (c.thr or 0) > 0.5 and so.pose == 0
	local fwd_speed = (c.thr or 0) * (sprint and S.SPRINT or base)
	if (c.thr or 0) < 0 then fwd_speed = (c.thr or 0) * base * 0.8 end
	local want = A.add(A.scale(f, fwd_speed), A.scale(right, (c.str or 0) * base * 0.85))
	local ground = W.ground_y(so.p)
	local airborne = so.p[2] > ground + 1e-4 or so.v[2] > 0
	if not airborne then
		so.v[1] = motor_step(so.v[1], want[1], dt)
		so.v[3] = motor_step(so.v[3], want[3], dt)
		if edge(so, 'jump', c.jump) then
			so.v[2] = S.JUMP_V
			airborne = true
			so.jump_t = M.t
		end
	else
		edge(so, 'jump', c.jump)
	end
	if airborne then
		local g = M.laws.gravity or 9.81
		local vy0 = so.v[2]
		so.v[2] = vy0 - g * dt
		so.p[1] = so.p[1] + so.v[1] * dt
		so.p[3] = so.p[3] + so.v[3] * dt
		so.p[2] = so.p[2] + (vy0 + so.v[2]) * 0.5 * dt
		local gy = W.ground_y(so.p)
		if so.p[2] <= gy then
			local impact = -so.v[2]
			so.p[2] = gy
			so.v[2] = 0
			so.v[1], so.v[3] = 0, 0
			S.fall_damage(so, impact)
		end
	else
		so.p[1] = so.p[1] + so.v[1] * dt
		so.p[3] = so.p[3] + so.v[3] * dt
	end
	so.moving = (so.v[1] ^ 2 + so.v[3] ^ 2) > 0.01
	so.jumping = airborne
	return c
end
S.edge = edge

-- fall damage curve [mock]: 0 below 9 m/s, 100 at 17 m/s, linear between
function S.fall_damage(so, v)
	if v <= 9 then return end
	local dmg = (v - 9) / 8 * 100
	M.WPN.damage(so, { damage = dmg, pos = A.copy(so.p), org = A.copy(so.p), dir = { 0, -1, 0 }, bone = 5, bullet = false,
		explosion = false, client = false, speed = v })
end

-- Health -----------------------------------------------------------------------------------------------------------------
function S.regen(so, dt)
	if not so.alive or so.hp >= 100 then return end
	local start = so.last_dmg_t
	if M.laws.supp_abort and so.last_supp_t > start then start = so.last_supp_t end
	if M.t - start >= (M.laws.regen_delay or 10) then
		so.hp = math.min(100.0, so.hp + (M.laws.regen_rate or 5) * dt)
	end
end

function S.immune(so)
	return M.t - so.spawn_t < (M.laws.immune_s or 2)
end

function S.suppress(so, by_pl)
	if M.laws.supp_effect == 'none' then return end
	so.last_supp_t = M.t
	so.supp_until = M.t + 3.0
	M.dispatch('Player:SuppressedEnemy', M.px(by_pl, 'Player'), M.px(so.pl, 'Player'))
end

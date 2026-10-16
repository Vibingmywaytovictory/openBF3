-- Mock human player (DESIGN.md 13.6): the scripted user's soldier in the server world. The Python user (user.py)
-- pushes tasks; U.step runs them inside the tick (before the soldiers step), so timing is per tick, not per chunk.
-- The human soldier goes through the same motor and weapon code as bots (so.uctl instead of an EntryInput); its
-- bullets carry isClientDamage = true. U.snapshot records per tick what the client realm needs (camera, weapon, gs).
mock = mock or {}
local M = mock
local A = M.A
local S = M.S
local W = M.W
local WPN = M.WPN

local U = {}
M.U = U

U.pl = nil
U.tasks = {}
U.cur = nil
U.ctl = { thr = 0, str = 0, fire = false, zoom = false }
U.snaps = {}
U.zoom_level = 0
U.log = {}          -- {k, what} for the tests
U.rng = nil

local function note(what)
	U.log[#U.log + 1] = { M.k, what }
end

function U.join(team)
	if U.pl ~= nil then return U.pl.id end
	local pl = S.new_player('user', math.tointeger(team) or 1, 1, 'human')
	U.pl = pl
	U.rng = U.rng or M.rng(424242)
	M.host_call(function() M.dispatch('Player:Authenticated', M.px(pl, 'Player')) end)
	note('join')
	return pl.id
end

function U.leave()
	local pl = U.pl
	if pl == nil then return end
	U.pl = nil
	U.tasks, U.cur = {}, nil
	M.host_call(function()
		if pl.so and pl.so.alive then S.die(pl.so, nil, 'left') end
		S.remove_player(pl)
		M.dispatch('Player:Left', M.px(pl, 'Player'))
	end)
	note('leave')
end

local function soldier()
	local pl = U.pl
	if pl and pl.so and pl.so.alive then return pl.so end
	return nil
end
U.soldier = soldier

local function select_asset(pl, key)
	local d = WPN.DEF[key]
	local a = M.ASSET[WPN.CANON[key]]
	if d and a then pl.sel[d.slot] = a end
end

-- spawn(pos, dir, key): the deploy screen: a soldier at pos with weapon key (plus the M9 secondary).
function U.spawn(pos, dir, key)
	local pl = U.pl
	if pl == nil then return false end
	pl.sel = {}
	select_asset(pl, key or (pl.teamId == 2 and 'AK74M' or 'M16A3'))
	select_asset(pl, 'M9')
	local f = A.norm({ dir[1], 0, dir[3] })
	local up = { 0, 1, 0 }
	local tr = M.lt(M.a3v(A.cross(up, f)), M.a3v(up), M.a3v(f), M.a3v(pos))
	local so = S.new_soldier(pl, tr)
	S.spawn(pl, so, tr, 0)
	so.aim = A.norm(dir)
	so.uctl = U.ctl
	U.ctl.aim = A.copy(so.aim)
	note('spawn')
	return true
end

function U.push(t)
	U.tasks[#U.tasks + 1] = t
end

function U.idle()
	return U.cur == nil and #U.tasks == 0
end

function U.clear()
	U.tasks, U.cur = {}, nil
	U.ctl.fire = false
end

local function shots_of(so)
	local w = so and WPN.current(so)
	return w and w.st.shots or 0
end

-- aim point with the user's noise (sd in m at the target) and an optional arm offset
local function aim_at(so, p, sd, arm)
	local e = S.eye(so)
	local q = A.copy(p)
	if sd and sd > 0 then
		q = { q[1] + sd * U.rng.n(), q[2] + sd * U.rng.n(), q[3] + sd * U.rng.n() }
	end
	if arm then
		local d = A.norm(A.sub(q, e))
		local r = A.norm(A.cross(d, { 0, 1, 0 }))
		q = A.add(q, A.scale(r, 0.3))
	end
	U.ctl.aim = A.norm(A.sub(q, e))
	so.aim = A.copy(U.ctl.aim)
end

local OPS = {}

function OPS.wait(t, so)
	t.until_t = t.until_t or (M.t + (t.s or 0))
	return M.t + 1e-9 >= t.until_t
end

function OPS.aim(t, so)
	if so == nil then return true end
	aim_at(so, t.p, t.sd, t.arm)
	return true
end

function OPS.aimdir(t, so)
	if so == nil then return true end
	U.ctl.aim = A.norm(t.d)
	so.aim = A.copy(U.ctl.aim)
	return true
end

function OPS.zoom(t, so)
	U.ctl.zoom = t.on == true
	U.zoom_level = t.level or (t.on and 1 or 0)
	return true
end

function OPS.move(t, so)
	if so == nil then return true end
	so.p = A.copy(t.p)
	so.v = { 0, 0, 0 }
	if t.dir then
		local f = A.norm({ t.dir[1], 0, t.dir[3] })
		if A.len(f) > 0.5 then so.body = f end
		U.ctl.aim = A.norm(t.dir)
		so.aim = A.copy(U.ctl.aim)
	end
	return true
end

-- fire: hold the trigger until n more shots (or timeout s), then release for one tick
function OPS.fire(t, so)
	if so == nil then
		U.ctl.fire = false
		return true
	end
	if t.n0 == nil then
		t.n0 = shots_of(so)
		t.t0 = M.t
		if t.target then aim_at(so, t.target, t.sd, t.arm) end
	end
	if shots_of(so) >= t.n0 + (t.n or 1) or M.t - t.t0 > (t.timeout or 10) then
		U.ctl.fire = false
		return true
	end
	U.ctl.fire = true
	return false
end

-- tap: n single shots, each a press and a release (gap s between)
function OPS.tap(t, so)
	if so == nil then
		U.ctl.fire = false
		return true
	end
	t.i = t.i or 0
	t.phase = t.phase or 'press'
	if t.i >= (t.n or 1) then
		U.ctl.fire = false
		return true
	end
	if t.phase == 'press' then
		if t.target then aim_at(so, t.target, t.sd, t.arm) end
		t.s0 = t.s0 or shots_of(so)
		U.ctl.fire = true
		t.phase = 'hold'
		t.t0 = M.t
	elseif t.phase == 'hold' then
		if shots_of(so) > t.s0 or M.t - t.t0 > 1.0 then
			U.ctl.fire = false
			t.phase = 'gap'
			t.t1 = M.t
		end
	else
		if M.t - t.t1 >= (t.gap or 0.5) then
			t.i = t.i + 1
			t.s0 = nil
			t.phase = 'press'
		end
	end
	return false
end

function OPS.reload(t, so)
	t.n = (t.n or 0) + 1
	U.ctl.reload = t.n <= 2
	if t.n > 2 then
		U.ctl.reload = false
		return true
	end
	return false
end

function OPS.equip(t, so)
	if so == nil then return true end
	WPN.swap(so, t.key)
	return true
end

function OPS.throw(t, so)
	if so == nil then return true end
	WPN.throw(so, t.p)
	local w = WPN.current(so)
	if w then w.st.shots = w.st.shots + 1 end
	return true
end

function OPS.suicide(t, so)
	if so then S.die(so, U.pl, 'suicide') end
	return true
end

function OPS.respawn(t, so)
	if so == nil then U.spawn(t.p, t.dir, t.key) end
	return true
end

function OPS.chat(t, so)
	local pl = U.pl
	if pl and M.laws.chat ~= false then M.dispatch('Player:Chat', M.px(pl, 'Player'), 0, t.text) end
	return true
end

-- step(dt): runs at the start of the simulation step of every tick
function U.step(dt)
	if U.pl == nil then return end
	local so = soldier()
	local guard = 0
	while guard < 8 do
		guard = guard + 1
		if U.cur == nil then
			U.cur = table.remove(U.tasks, 1)
			if U.cur == nil then break end
		end
		local fn = OPS[U.cur.op]
		if fn == nil then error('mock user: unknown op ' .. tostring(U.cur.op)) end
		if fn(U.cur, so) then
			U.cur = nil
		else
			break
		end
	end
	if so then
		so.uctl = U.ctl
		if U.ctl.aim then so.aim = A.copy(U.ctl.aim) end
	end
end

-- snapshot(): per tick, what the client realm sees of the local player
function U.snapshot()
	local pl = U.pl
	if pl == nil then return end
	local so = soldier()
	local s = { k = M.k, alive = so ~= nil, pid = pl.id, team = pl.teamId }
	if so then
		s.eye = S.eye(so)
		s.aim = A.copy(so.aim)
		s.pose = so.pose
		s.hp = so.hp
		s.zoom = (so.zoom and U.zoom_level > 0) and U.zoom_level or 0
		local w = WPN.current(so)
		if w then
			local g = w.wf.gs
			s.w = { name = w.name, ammo = w.primaryAmmo, iid = w.wf.instanceId, gs = { da = g.dispersionAngle,
				dmin = g.minDispersionAngle, fs = g.fireShot == true, fir = g.isFiring == true, tsls = g.timeSinceLastShot,
				ra = g.randomAngle, rr = g.randomRadius, seed = g.seed, ifs = g.initialFireShot, rp = g.dev_rec.pitch,
				ry = g.dev_rec.yaw } }
		end
	end
	U.snaps[#U.snaps + 1] = s
end

function U.take_snaps()
	local out = U.snaps
	U.snaps = {}
	return out
end

function U.take_log()
	local out = U.log
	U.log = {}
	return out
end

function U.state()
	local so = soldier()
	return { joined = U.pl ~= nil, alive = so ~= nil, idle = U.idle(), pid = U.pl and U.pl.id or -1,
		shots = shots_of(so), hp = so and so.hp or 0, pos = so and A.copy(so.p) or nil }
end

-- bot_point(sid, zone): a point on a bot soldier (feet + zone height), nil if it is not alive
U.ZONE_Y = { chest = 1.3, head = 1.75, legs = 0.5, stomach = 0.92, arm = 1.3 }
function U.bot_point(sid, zone)
	for _, p in ipairs(S.players) do
		if p.__sid == sid and p.so and p.so.alive then
			local so = p.so
			local h = S.height(so) / 1.85
			return { so.p[1], so.p[2] + (U.ZONE_Y[zone or 'chest'] or 1.3) * h, so.p[3] }
		end
	end
	return nil
end

function U.player_count()
	return #S.players
end

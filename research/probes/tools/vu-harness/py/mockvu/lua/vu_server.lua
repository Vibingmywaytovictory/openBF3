-- Mock VU server realm (DESIGN.md 13.2): the server libraries, constructors and casts the mod sees, the tick loop,
-- levels and round restarts, the runtime observe-only checks (13.4) and the host API called from host.py.
mock = mock or {}
local M = mock
local A = M.A
local W = M.W
local S = M.S
local WPN = M.WPN
local C = M.C

M.realm = 'server'
M.viol = {}            -- observe-only violations (strings)
M.counts = { writes = 0, calls = 0, rcon = 0, ticks = 0 }
M.pending_calls = {}   -- fixture calls of the current tick {sid, field}
M.pending_admin = 0
M.inbox = {}           -- chat messages to humans: {k, pid, text, yell}
M.host_q = {}          -- host actions to run at the start of the next tick
M.level = nil
M.level_state = 'none'
M.sched = {}           -- {t, fn}
M.marks = {}           -- mock-side events for the tests {k, what}
M.out_scan = 1

local function viol(what)
	if #M.viol < 200 then M.viol[#M.viol + 1] = string.format('k%d %s', M.k, what) end
end
M.viol_add = viol

function M.mark_event(what)
	M.marks[#M.marks + 1] = { M.k, what }
end

-- Journal checks (DESIGN 13.4) -------------------------------------------------------------------------------------------
function M.on_journal(e)
	if e.kind == 'write' then
		M.counts.writes = M.counts.writes + 1
		local ok = false
		if M.BOT_ASSIGN[e.field] then
			if e.view == 'EntryInput' and e.owner == 'input' then ok = true end
			if e.view == 'Player' and e.owner == 'bot' and (e.field == 'input' or e.field == 'selectedKit') then ok = true end
		end
		if not ok then viol('write ' .. e.view .. '.' .. tostring(e.field) .. ' owner ' .. tostring(e.owner)) end
	elseif e.kind == 'call' then
		M.counts.calls = M.counts.calls + 1
		if e.field ~= 'SetLevel' then
			if e.owner == 'human' then viol('fixture ' .. e.field .. ' on a human') end
			M.pending_calls[#M.pending_calls + 1] = { sid = e.sid, field = e.field }
		end
	end
end

-- end-of-tick check: every fixture call has an fx row for that bot in the same tick; admin RCON has fx act=admin.
local function scan_tick_rows()
	local fx = {}
	local admin = 0
	for i = M.out_scan, #M.out do
		local line = M.out[i][2]
		if line:find('MH1 {', 1, true) then
			if line:find('"e":"fx"', 1, true) then
				local id = line:match('"id":(%d+)')
				if id then fx[tonumber(id)] = true end
				if line:find('"act":"admin"', 1, true) then admin = admin + 1 end
			elseif line:find('"e":"mark"', 1, true) then
				local lbl = line:match('"lbl":"([^"]*)"')
				local sc = line:match('"sc":"([^"]*)"')
				local idx = line:match('"i":(%d+)')
				if lbl == 'cfg-start' then M.on_cfg_start(sc, tonumber(idx)) end
				if lbl == 'cfg-done' or lbl == 'cfg-skip' then M.spawn_block = false end
			end
		end
	end
	M.out_scan = #M.out + 1
	for _, c in ipairs(M.pending_calls) do
		if c.sid == nil or not fx[c.sid] then viol('fixture ' .. c.field .. ' without fx row (sid ' .. tostring(c.sid) .. ')') end
	end
	M.pending_calls = {}
	if M.pending_admin > admin then viol('admin RCON without fx act=admin') end
	M.pending_admin = 0
end

function M.on_cfg_start(sc, i)
	M.spawn_block = false
	local sf = M.laws.spawn_fails
	if type(sf) == 'string' and sf == string.format('%s:%d', tostring(sc), i or -1) then M.spawn_block = true end
	local wo = M.laws.w1_outliers
	if type(wo) == 'string' and sc == 'WA' then
		local k, f = wo:match('^(%d+)@([%d.]+)$')
		M.out_left, M.out_f = tonumber(k), tonumber(f)
	end
end

-- Objects the mod can reach ------------------------------------------------------------------------------------------------
local function lib(name)
	return M.px({ __cls = name, __owner = 'none' }, name)
end

local function new_input()
	local o = { __cls = 'EntryInput', __owner = 'input', deltaTime = 0.0, flags = 0, authoritativeAimingYaw = 0.0,
		authoritativeAimingPitch = 0.0, zoomLevel = 0, levels = {} }
	o.authoritativeCameraPosition = M.vec3(0, 0, 0)
	return M.px(o, 'EntryInput')
end

local function guid()
	local o = { __cls = 'Guid', __owner = 'none' }
	o.__tostring = function() return '6E1D7C2A-4B3F-4A8E-9C1D-2F5B8E7A9C40' end
	return M.px(o, 'Guid')
end

M.IMPL.Events = {
	Subscribe = function(_, name, a, b) return M.subscribe(name, a, b) end,
	Unsubscribe = function(_, name)
		for _, s in ipairs(M.subs[name] or {}) do s.gone = true end
	end,
}
M.IMPL.Hooks = { Install = function(_, name, prio, a, b) return M.hook_install(name, prio, a, b) end }
M.IMPL.NetEvents = {
	Subscribe = function(_, name, a, b) return M.net_subscribe(name, a, b) end,
	SendTo = function(_, name, pl, ...)
		local r = M.raw(pl)
		if r == nil then error('mock: NetEvents:SendTo needs a player', 2) end
		M.net_send(name, r.id, ...)
	end,
	Broadcast = function(_, name, ...) M.net_send(name, -1, ...) end,
}

M.IMPL.ChatManager = {
	SendMessage = function(_, msg, pl)
		if type(msg) ~= 'string' then error('mock: SendMessage needs a string', 2) end
		if #msg > 127 then viol('chat message over 127 chars') end
		if M.laws.chat == false then return false end
		local pid = pl and M.raw(pl).id or -1
		M.inbox[#M.inbox + 1] = { M.k, pid, msg, false }
		return true
	end,
	Yell = function(_, msg, dur, pl)
		if M.laws.chat == false then return end
		local pid = pl and M.raw(pl).id or -1
		M.inbox[#M.inbox + 1] = { M.k, pid, msg, true }
	end,
}

M.IMPL.PlayerManager = {
	GetPlayers = function()
		local out = {}
		for _, p in ipairs(S.players) do out[#out + 1] = M.px(p, 'Player') end
		return out
	end,
	GetPlayerById = function(_, id)
		local p = S.by_id(id)
		return p and M.px(p, 'Player') or nil
	end,
	GetPlayerCount = function() return #S.players end,
	CreatePlayer = function(_, name, team, squad)
		if type(name) ~= 'string' then error('mock: CreatePlayer needs a name', 2) end
		local p = S.new_player(name, math.tointeger(team) or 0, math.tointeger(squad) or 0, 'bot')
		return M.px(p, 'Player')
	end,
	DeletePlayer = function(_, pl)
		local r = M.raw(pl)
		if r == nil then error('mock: DeletePlayer needs a player', 2) end
		if r.__owner ~= 'bot' then viol('DeletePlayer on a non-bot') end
		if r.so and r.so.alive then S.die(r.so, nil, 'removed') end
		C.on_remove(r)
		S.remove_player(r)
		M.dispatch('Player:Left', M.px(r, 'Player'))
	end,
}

local VARS = {
	['vars.serverName'] = 'mh-measure', ['vars.soldierHealth'] = '100', ['vars.regenerateHealth'] = 'true',
	['vars.bulletDamage'] = '100', ['vars.playerRespawnTime'] = '100', ['vars.gameModeCounter'] = '100',
	['vars.friendlyFire'] = 'false', ['vars.autoBalance'] = 'false', ['vars.idleTimeout'] = '0',
	['vars.roundStartPlayerCount'] = '4', ['vars.roundRestartPlayerCount'] = '2', ['vars.vehicleSpawnAllowed'] = 'true',
	['vu.SuppressionMultiplier'] = '100', ['vu.TimeScale'] = '1.000000', ['vu.DisablePreRound'] = 'false',
}

M.IMPL.RCON = {
	SendCommand = function(_, cmd, args)
		M.counts.rcon = M.counts.rcon + 1
		if type(cmd) ~= 'string' then
			viol('RCON non-string command')
			return { 'InvalidArguments' }
		end
		if args ~= nil then viol('RCON ' .. cmd .. ' with arguments') end
		if M.RCON_ADMIN[cmd] then
			M.pending_admin = M.pending_admin + 1
			if cmd == 'mapList.restartRound' then M.schedule_level('rr', 0.5) else M.schedule_level('next', 0.5) end
			return { 'OK' }
		end
		if not M.RCON_READ[cmd] then
			viol('RCON ' .. cmd .. ' not whitelisted')
			return { 'UnknownCommand' }
		end
		if cmd == 'version' then return { 'OK', 'VU', '20682' } end
		if cmd == 'modList.ListRunning' then return { 'OK', 'mharness' } end
		if cmd == 'serverInfo' then return { 'OK', 'mh-measure', tostring(#S.players), '64' } end
		if cmd == 'vu.FrequencyMode' then return { 'OK', M.hz == 60 and 'high60' or 'regular' } end
		if M.laws.rcon_vars == false then return { 'UnknownCommand' } end
		local v = VARS[cmd]
		if v == nil then return { 'UnknownCommand' } end
		return { 'OK', v }
	end,
	GetServerGuid = function() return guid() end,
}

M.IMPL.TicketManager = {
	GetTicketCount = function(_, team) return C.tickets[math.tointeger(team) or 0] or 0.0 end,
	GetCurrentRound = function() return C.round end,
	GetRoundCount = function() return 1 end,
}

local function raycast_impl(from, to, maxhits, matflags, flags)
	if not M.is_vec3(from) or not M.is_vec3(to) then error('mock: raycast needs Vec3 from/to', 3) end
	flags = math.tointeger(flags) or 0
	local chars = nil
	if (flags & 32) == 0 then chars = S.alive_soldiers() end
	local hits = W.raycast(M.v3a(from), M.v3a(to), math.tointeger(maxhits) or 1, chars)
	local out = {}
	for i, h in ipairs(hits) do
		local rb = h.so and M.px(h.so, 'Entity') or (h.ent and M.px(h.ent, 'Entity') or nil)
		out[i] = M.px({ __cls = 'RayCastHit', __owner = 'none', position = M.a3v(h.pos), normal = M.a3v(h.n),
			material = h.mat and M.px(h.mat, 'DataContainer') or nil, part = -1, bone = -1, rigidBody = rb }, 'RayCastHit')
	end
	return out
end
M.IMPL.RaycastManager = {
	DetailedRaycast = function(_, from, to, maxhits, matflags, flags) return raycast_impl(from, to, maxhits, matflags, flags) end,
	CollisionRaycast = function(_, from, to, maxhits, matflags, flags) return raycast_impl(from, to, maxhits, matflags, flags) end,
}

M.ASSET = {}
function M.build_assets()
	M.ASSET = {}
	local function add(name, key, cls)
		local o = { __cls = 'DataContainer', __owner = 'none', aname = name, key = key, __dtype = cls }
		o.__get = { typeInfo = function() return M.typeinfo(cls) end }
		M.ASSET[name] = o
	end
	for low, key in pairs(WPN.ASSETS) do add(WPN.CANON[key], key, 'SoldierWeaponUnlockAsset') end
	for _, n in ipairs(WPN.OTHER_ASSETS) do add(n, nil, 'DataContainer') end
end

M.IMPL.ResourceManager = {
	SearchForDataContainer = function(_, name)
		if type(name) ~= 'string' then return nil end
		local hit = M.ASSET[name]
		if hit == nil and M.laws.asset_case ~= 'exact' then
			local low = name:lower()
			for n, o in pairs(M.ASSET) do if n:lower() == low then hit = o end end
		end
		return hit and M.px(hit, 'DataContainer') or nil
	end,
}

local function iter_list(name)
	local items = {}
	if M.laws.iter_types ~= false then
		for _, e in ipairs(W.ents) do if e.__etype == name and not e.destroyed then items[#items + 1] = e end end
	end
	return items
end

M.IMPL.EntityManager = {
	GetIterator = function(_, name)
		local it = { __cls = 'EntityIterator', __owner = 'none', items = iter_list(name), i = 0 }
		return M.px(it, 'EntityIterator')
	end,
	TraverseAllEntities = function(_, fn)
		local snap = {}
		for i, e in ipairs(W.ents) do snap[i] = e end
		for _, e in ipairs(snap) do fn(M.px(e, 'Entity')) end
	end,
}
M.IMPL.EntityIterator = {
	Next = function(it)
		it.i = it.i + 1
		local e = it.items[it.i]
		return e and M.px(e, 'Entity') or nil
	end,
}
M.IMPL.EntityBusPeer = { Is = function(o, name) return M.is(o, name) end }
M.IMPL.DataContainer = { Is = function(o, name) return M.is(o, name) end }
M.IMPL.EntryInput = {
	SetLevel = function(o, a, l)
		a = math.tointeger(a)
		if a == nil or type(l) ~= 'number' then error('mock: SetLevel(action, level) needs numbers', 2) end
		o.levels[a] = l
	end,
	GetLevel = function(o, a) return o.levels[math.tointeger(a) or -1] or 0.0 end,
}

M.IMPL.SharedUtils = {
	GetTimeMS = function() return M.ms() end,
	GetTime = function() return M.t end,
	GetTickrate = function() return M.hz + 0.0 end,
	GetLevelName = function() return M.level and M.level.name or nil end,
	GetCurrentGameMode = function() return M.level and M.level.mode or nil end,
	IsServerModule = function() return true end,
	IsClientModule = function() return false end,
}

local function bot_only(o, what)
	if o.__owner ~= 'bot' then viol(what .. ' on a non-bot') return false end
	return true
end

M.IMPL.Player = {
	SelectWeapon = function(o, slot, unlock, unlocks)
		if not bot_only(o, 'SelectWeapon') then return end
		local a = M.raw(unlock)
		if a == nil then error('mock: SelectWeapon needs an unlock asset', 2) end
		o.sel[math.tointeger(slot) or 0] = a
	end,
	SelectUnlockAssets = function(o, cust, list)
		bot_only(o, 'SelectUnlockAssets')
	end,
	CreateSoldier = function(o, bp, tr)
		if not bot_only(o, 'CreateSoldier') then return nil end
		if not M.is_lt(tr) then error('mock: CreateSoldier needs a LinearTransform', 2) end
		if M.raw(bp) == nil then error('mock: CreateSoldier needs a blueprint', 2) end
		if M.level_state ~= 'loaded' then return nil end
		local so = S.new_soldier(o, tr)
		return M.px(so, 'SoldierEntity')
	end,
	SpawnSoldierAt = function(o, sop, tr, pose)
		if not bot_only(o, 'SpawnSoldierAt') then return end
		local so = M.raw(sop)
		if so == nil or not M.is_lt(tr) then error('mock: SpawnSoldierAt(soldier, transform, pose)', 2) end
		if M.spawn_block or M.level_state ~= 'loaded' then return end
		S.spawn(o, so, tr, pose)
	end,
	EnterVehicle = function(o) bot_only(o, 'EnterVehicle') end,
	ExitVehicle = function(o) bot_only(o, 'ExitVehicle') end,
}

M.IMPL.SoldierEntity = {
	SetPosition = function(so, p)
		if not bot_only(so, 'SetPosition') then return end
		if not M.is_vec3(p) then error('mock: SetPosition needs a Vec3', 2) end
		S.set_position(so, M.v3a(p))
	end,
	SetTransform = function(so, tr)
		if not bot_only(so, 'SetTransform') then return end
		if not M.is_lt(tr) then error('mock: SetTransform needs a LinearTransform', 2) end
		S.set_position(so, M.v3a(tr.trans))
		local f = A.norm({ tr.forward.x, 0, tr.forward.z })
		if A.len(f) > 0.5 then so.body = f end
	end,
	SetPose = function(so, pose, imm, over)
		if not bot_only(so, 'SetPose') then return end
		so.pose = math.tointeger(pose) or 0
	end,
	Kill = function(so)
		if not bot_only(so, 'Kill') then return end
		S.die(so, nil, 'kill')
	end,
}
M.IMPL.ControllableEntity = {
	GetPlayerInEntry = function(so) return M.px(so.pl, 'Player') end,
	GetDataForEntry = function() return nil end,
}
M.IMPL.PhysicsEntityBase = { GetPartTransform = function() return M.lt() end }

-- Globals for the mod environment -------------------------------------------------------------------------------------------
function M.server_globals(inv)
	local g = {}
	for _, n in ipairs({ 'Events', 'Hooks', 'NetEvents', 'ChatManager', 'PlayerManager', 'EntityManager', 'SharedUtils', 'RCON',
		'TicketManager', 'RaycastManager', 'ResourceManager' }) do g[n] = lib(n) end
	g.Vec3 = function(x, y, z) return M.vec3(x, y, z) end
	g.LinearTransform = function(...) return M.lt(...) end
	g.EntryInput = new_input
	for _, t in ipairs({ 'SoldierEntity', 'CapturePointEntity', 'TicketCounterEntity', 'SpatialEntity', 'MaterialContainerPair',
		'ControllableEntity', 'PhysicsEntityBase', 'EntityBusPeer', 'Player' }) do
		g[t] = function(x) return M.cast(t, x) end
	end
	g.json = M.json
	g.class = function() error('mock: class() is not used by the mod', 2) end
	for name, tbl in pairs(M.enum_tables()) do g[name] = tbl end
	return g
end

-- Levels ----------------------------------------------------------------------------------------------------------------------
M.levels = {}      -- from levels.py, in map-list order
M.level_i = 1
M.level_opts = {}

function M.schedule(dt, fn)
	M.sched[#M.sched + 1] = { t = M.t + dt, fn = fn }
end

function M.schedule_level(kind, delay)
	if M.level_state ~= 'loaded' then return end
	M.level_state = 'pending'
	M.schedule(delay or 0.5, function()
		M.level_state = 'loading'
		-- every soldier leaves the world; players stay (rr_state=lost also reloads the Lua state)
		for _, p in ipairs(S.players) do
			if p.so and p.so.alive then
				p.so.alive = false
				W.remove_ent(p.so)
				p.so = nil
			end
		end
		WPN.proj = {}
		M.dispatch('Level:Destroy')
		M.mark_event('level-destroy')
		if kind == 'next' then M.level_i = M.level_i % #M.levels + 1 end
		if M.laws.rr_state == 'lost' then M.reload_mod_pending = true end
		M.schedule(2.0, function()
			if M.reload_mod_pending then
				M.reload_mod_pending = false
				M.reload_mod()
			end
			M.load_level(M.level_i, false)
		end)
	end)
end

function M.load_level(i, first)
	local L = M.levels[i]
	M.level = L
	W.reset(L)
	C.reset(L, M.level_opts)
	C.round = 0
	M.level_state = 'loaded'
	M.mark_event('level-loaded ' .. L.name)
	M.dispatch('Level:Loaded', L.name, L.mode, 0, 1)
	if M.on_level_loaded_host then M.on_level_loaded_host(L.name) end
end

-- Mod loading (host API) ---------------------------------------------------------------------------------------------------
M.sources = nil
M.order = { '__shared/__init__', '__init__' }
M.vm = 'same'

function M.reload_mod()
	M.subs, M.hooks, M.net_subs = {}, {}, {}
	M.mark_event('mod-reload')
	local errs = M.load_mod(M.sources, 'server', M.order, M.vm, M.server_globals())
	for _, e in ipairs(errs) do M.errors[#M.errors + 1] = 'load: ' .. e end
end

function M.start_mod(sources, order, vm)
	M.sources = sources
	M.order = order or M.order
	M.vm = vm or 'same'
	M.build_assets()
	local errs = M.load_mod(M.sources, 'server', M.order, M.vm, M.server_globals())
	for _, e in ipairs(errs) do M.errors[#M.errors + 1] = 'load: ' .. e end
	return #errs
end

-- Tick loop -----------------------------------------------------------------------------------------------------------------
local function run_sched()
	local due = {}
	local keep = {}
	for _, s in ipairs(M.sched) do
		if M.t + 1e-9 >= s.t then due[#due + 1] = s else keep[#keep + 1] = s end
	end
	M.sched = keep
	for _, s in ipairs(due) do s.fn() end
end

function M.nsub()
	local c = M.laws.gs_cadence or 'tick'
	local n = tonumber(tostring(c):match('^frame:(%d+)$') or '')
	return n or 1
end

M.snaps = {}

function M.tick()
	M.k = M.k + 1
	M.t = M.k * M.dt
	M.counts.ticks = M.counts.ticks + 1
	run_sched()
	-- host actions (chat, NetEvents from the client, user joins) run inside the tick
	local q = M.host_q
	M.host_q = {}
	for _, fn in ipairs(q) do handler_ok = M.handler_call('host', fn) end
	M.dispatch('UpdateManager:Update', M.dt, 0)
	if M.level_state ~= 'loading' then
		local dt = M.dt
		if M.U then M.U.step(dt) end
		local nsub = M.nsub()
		for _, so in ipairs(S.alive_soldiers()) do S.step(so, dt) end
		for _, so in ipairs(S.alive_soldiers()) do WPN.update(so, dt, nsub) end
		WPN.step_projectiles(dt)
		for _, so in ipairs(S.alive_soldiers()) do S.regen(so, dt) end
		C.step(dt)
		if M.U then M.U.snapshot() end
	end
	M.dispatch('UpdateManager:Update', M.dt, 1)
	M.dispatch('Engine:Update', M.dt, M.dt)
	scan_tick_rows()
end

function M.run_ticks(n)
	for _ = 1, n do M.tick() end
	return M.k
end

-- idle(n): the server does not tick (idle_advance=no with nobody connected); wall time still passes.
function M.idle(n)
	M.wall0 = M.wall0 + n * M.dt
	M.ms0 = M.ms0 + math.floor(n * M.dt * 1000 + 0.5)
end

function M.host_call(fn)
	M.host_q[#M.host_q + 1] = fn
end

function M.take_inbox()
	local out = M.inbox
	M.inbox = {}
	return out
end

function M.take_viol()
	local out = M.viol
	M.viol = {}
	return out
end

function M.take_marks()
	local out = M.marks
	M.marks = {}
	return out
end

function M.take_errors()
	local out = M.errors
	M.errors = {}
	return out
end

-- chat(pid, text): the user typed in chat (Player:Chat only with chat=on).
function M.chat(pid, text)
	M.host_call(function()
		local p = S.by_id(pid)
		if p == nil then return end
		if M.laws.chat == false then return end
		M.dispatch('Player:Chat', M.px(p, 'Player'), 0, text)
	end)
end

-- deliver(name, pid, args): a NetEvent from the client of player pid.
function M.deliver(name, pid, args)
	M.host_call(function()
		local p = S.by_id(pid)
		if p == nil then return end
		M.net_deliver(name, M.px(p, 'Player'), args)
	end)
end

function M.stats()
	local bots, humans = 0, 0
	for _, p in ipairs(S.players) do
		if p.__owner == 'bot' then bots = bots + 1 else humans = humans + 1 end
	end
	return { k = M.k, t = M.t, bots = bots, humans = humans, writes = M.counts.writes, calls = M.counts.calls,
		rcon = M.counts.rcon, t1 = C.tickets[1], t2 = C.tickets[2], level = M.level and M.level.name or '',
		state = M.level_state, proj = #WPN.proj }
end

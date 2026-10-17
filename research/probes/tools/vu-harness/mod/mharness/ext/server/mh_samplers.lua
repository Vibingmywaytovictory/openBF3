-- Samplers: hp, wf, gs (event and poll), tk, cp and the watched set (DESIGN.md section 3.10). Read only.
-- TicketManager:GetTicketCount (server/library/ticketmanager.md); ServerTicketCounterEntity and
-- ServerCapturePointEntity iterators (shared/library/entitymanager.md, content/vext/entities.md;
-- server/type/ticketcounterentity.md, server/type/capturepointentity.md) [live:L15]; GunSway:Update
-- (shared/event/gunsway_update.md) [live:L11]; WeaponFiring.gunSway (RO, shared/type/weaponfiring.md) for src=poll.
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local G = require('__shared/mh_gs')
local ids = require('mh_ids')
local bots = require('mh_bots')

local M = {}

M.watched = {}     -- sid -> {hp=bool, gs=bool, wf=bool, poll=bool, until_k=int|nil}
M.inst = {}        -- WeaponFiring.instanceId -> sid (refreshed per tick for gs watchers)
M.nup = {}         -- sid -> updates this tick
M.nup_k = {}
M.wf_sig = {}
M.counters = {}    -- sid -> {fs=n, shot=n, hit=n, fs_seen=bool}
M.tk_mode = '1hz'
M.cp_mode = 'change'
M.cp_active = {}   -- cp id -> true (per tick)
M.cp_sig = {}
M.flag_list = nil
M.tc_list = nil
M.gs_every = 1
M.gs_srv_seen = 0
M.gs_poll_seen = 0
M.srv_last = {}    -- sid -> last srv payload signature (P0.7 agreement)
M.agree = { n = 0, ok = 0 }
M.last_gs = {}     -- sid -> last gs payload (scenario logic: SETTLE, P0.11)

function M.counter(sid)
	local c = M.counters[sid]
	if not c then
		c = { fs = 0, shot = 0, hit = 0, fs_seen = false, shot_seen = false }
		M.counters[sid] = c
	end
	return c
end

-- shots(sid): attributed shot count: gs.fs, else shot rows, else hit rows (DESIGN 6.2).
function M.shots(sid)
	local c = M.counter(sid)
	if c.fs_seen then return c.fs end
	if c.shot_seen then return c.shot end
	return c.hit
end

function M.watch(sid, what, until_k)
	local w = M.watched[sid] or {}
	for key, val in pairs(what or { hp = true }) do w[key] = val end
	w.until_k = until_k
	M.watched[sid] = w
end

function M.unwatch(sid)
	M.watched[sid] = nil
end

function M.unwatch_all()
	M.watched = {}
end

local function player_of(sid)
	local b = bots.bots[sid]
	if b then return b.pl end
	return ids.get(sid)
end

local function soldier_of(sid)
	local pl = player_of(sid)
	if pl == nil then return nil end
	local ok, so = pcall(function() return pl.soldier end)
	if ok then return so end
	return nil
end
M.soldier_of = soldier_of

local function pose_zoom(so)
	local pose, zoom = 0, false
	pcall(function()
		pose = math.tointeger(so.pose) or 0
		zoom = so.weaponsComponent.isZooming == true
	end)
	return pose, zoom
end

function M.hp_row(sid, so, extra)
	local wt = so.worldTransform
	local vel = nil
	pcall(function() vel = V.v(so.velocity) end)
	R.row('hp', { id = sid, hp = so.health, pose = math.tointeger(so.pose) or 0, pos = V.v(wt.trans),
		alive = so.isAlive == true, ppose = math.tointeger(so.pendingPose), vel = vel, md = so.isManDown == true,
		ias = extra })
end

local function wf_row(sid, so)
	local w = so.weaponsComponent.currentWeapon
	if w == nil then return end
	local f = w.weaponFiring
	if f == nil then return end
	local ws, wsl, wsn = math.tointeger(f.weaponState) or -1, math.tointeger(f.lastWeaponState) or -1,
		math.tointeger(f.nextWeaponState) or -1
	local ammo, ammo2 = math.tointeger(w.primaryAmmo) or -1, math.tointeger(w.secondaryAmmo) or -1
	local fl = math.tointeger(w.fireLogic) or -1
	local slot = math.tointeger(so.weaponsComponent.currentWeaponSlot) or -1
	local sig = ws .. ':' .. wsl .. ':' .. wsn .. ':' .. ammo .. ':' .. ammo2 .. ':' .. fl .. ':' .. slot
	if M.wf_sig[sid] == sig then return end
	M.wf_sig[sid] = sig
	R.row('wf', { id = sid, wid = ids.weapon(w.name), ws = ws, wsl = wsl, wsn = wsn, ttw = f.timeToWait,
		rt = f.recoilTimer, ra = { f.recoilAngleX, f.recoilAngleY, f.recoilAngleZ }, ammo = ammo, ammo2 = ammo2,
		fl = fl, slot = slot })
end

local function gs_poll(sid, so, w)
	local f = w.weaponFiring
	if f == nil then return end
	local g = f.gunSway
	if g == nil then return end
	local pose, zoom = pose_zoom(so)
	local p = G.payload(g, sid, ids.weapon(w.name), 'poll', pose, zoom, 1, C.dt(), math.tointeger(w.fireLogic))
	M.gs_poll_seen = M.gs_poll_seen + 1
	if not M.counter(sid).fs_seen_srv then M.last_gs[sid] = p end
	local last = M.srv_last[sid]
	if last and last.k == C.gk then
		M.agree.n = M.agree.n + 1
		if math.abs((last.da or 0) - (p.da or 0)) < 1e-6 and last.fs == p.fs then M.agree.ok = M.agree.ok + 1 end
	end
	if not M.counter(sid).fs_seen_srv and p.fs then
		local c = M.counter(sid)
		c.fs = c.fs + 1
		c.fs_seen = true
	end
	R.row('gs', p)
end

-- on_postsim(): per tick, after the clock advanced.
function M.on_postsim()
	local k = C.gk
	local hz = C.hzv
	M.inst = {}
	for sid, w in pairs(M.watched) do
		if w.until_k and k > w.until_k then
			M.watched[sid] = nil
		else
			local so = soldier_of(sid)
			if so ~= nil then
				if w.hp then R.safe('smp.hp', M.hp_row, sid, so) end
				local wpn = nil
				pcall(function() wpn = so.weaponsComponent.currentWeapon end)
				if wpn ~= nil then
					if w.wf then R.safe('smp.wf', wf_row, sid, so) end
					if w.gs then
						local okf, f = pcall(function() return wpn.weaponFiring end)
						if okf and f ~= nil then
							local oki, iid = pcall(function() return f.instanceId end)
							if oki and iid ~= nil then M.inst[iid] = sid end
						end
						if w.poll ~= false and (k % M.gs_every == 0) then R.safe('smp.gspoll', gs_poll, sid, so, wpn) end
					end
				end
			end
		end
	end
	-- 1 Hz hp for unwatched bots
	if k % hz == 0 then
		for sid, b in pairs(bots.bots) do
			if not M.watched[sid] then
				local so = bots.soldier(b)
				if so ~= nil then R.safe('smp.hp1', M.hp_row, sid, so) end
			end
		end
	end
	if M.tk_mode == 'tick' or (M.tk_mode == '1hz' and k % hz == 0) then R.safe('smp.tk', M.tk_row) end
	if M.cp_mode ~= 'off' then R.safe('smp.cp', M.cp_rows, k % hz == 0) end
end

-- GunSway:Update(gunSway, weapon|nil, weaponFiring|nil, deltaTime): src=srv for watched instances.
function M.on_gunsway(g, weapon, wf, dt)
	if wf == nil or next(M.inst) == nil then return end
	local iid = wf.instanceId
	local sid = M.inst[iid]
	if sid == nil then return end
	M.gs_srv_seen = M.gs_srv_seen + 1
	local k = C.gk
	if M.nup_k[sid] ~= k then
		M.nup_k[sid] = k
		M.nup[sid] = 0
	end
	M.nup[sid] = M.nup[sid] + 1
	local so = soldier_of(sid)
	local pose, zoom = 0, false
	local fl = nil
	if so ~= nil then
		pose, zoom = pose_zoom(so)
		pcall(function() fl = math.tointeger(so.weaponsComponent.currentWeapon.fireLogic) end)
	end
	local wname = nil
	pcall(function() wname = so.weaponsComponent.currentWeapon.name end)
	local p = G.payload(g, sid, ids.weapon(wname), 'srv', pose, zoom, M.nup[sid], dt, fl)
	M.srv_last[sid] = { k = k, da = p.da, fs = p.fs }
	M.last_gs[sid] = p
	if p.fs then
		local c = M.counter(sid)
		c.fs = c.fs + 1
		c.fs_seen = true
		c.fs_seen_srv = true
	end
	R.row('gs', p)
end

-- Tickets and flags --------------------------------------------------------------------------------------------

local function iterate(type_name, cast)
	local out = {}
	local ok = pcall(function()
		local it = EntityManager:GetIterator(type_name)
		if it == nil then return end
		local e = it:Next()
		while e ~= nil do
			out[#out + 1] = cast(e)
			e = it:Next()
		end
	end)
	if not ok then out = {} end
	return out
end

function M.flags()
	if M.flag_list == nil then
		local list = iterate('ServerCapturePointEntity', function(e) return CapturePointEntity(e) end)
		if #list == 0 then
			-- Fallback (L15): traverse every entity and test the type name.
			pcall(function()
				EntityManager:TraverseAllEntities(function(e)
					if e:Is('ServerCapturePointEntity') then list[#list + 1] = CapturePointEntity(e) end
				end)
			end)
		end
		M.flag_list = list
		for _, cp in ipairs(list) do ids.flag(cp) end
	end
	return M.flag_list
end

function M.counters_list()
	if M.tc_list == nil then
		local list = iterate('ServerTicketCounterEntity', function(e) return TicketCounterEntity(e) end)
		if #list == 0 then
			pcall(function()
				EntityManager:TraverseAllEntities(function(e)
					if e:Is('ServerTicketCounterEntity') then list[#list + 1] = TicketCounterEntity(e) end
				end)
			end)
		end
		M.tc_list = list
	end
	return M.tc_list
end

function M.tickets(team)
	local ok, v = pcall(function() return TicketManager:GetTicketCount(team) end)
	if ok and type(v) == 'number' then return v end
	return nil
end

function M.initial_tickets(team)
	for _, tc in ipairs(M.counters_list()) do
		local ok, tm, ini = pcall(function() return tc.team, tc.initialTicketCount end)
		if ok and math.tointeger(tm) == team and type(ini) == 'number' then return ini end
	end
	return nil
end

function M.tk_row()
	local tm = {}
	for team = 1, 2 do
		local tf = M.tickets(team) or -1
		local tc, cav = -1, -1
		for _, c in ipairs(M.counters_list()) do
			pcall(function()
				if math.tointeger(c.team) == team then
					tc = c.ticketCount
					cav = c.controlledAreaValue
				end
			end)
		end
		tm[#tm + 1] = { team, tf, tc, cav }
	end
	R.row('tk', { tm = tm })
end

function M.inside_counts(cp)
	local n1, n2 = 0, 0
	local ok, list = pcall(function() return cp.playersInside end)
	if ok and list then
		for _, pl in ipairs(list) do
			local okt, t = pcall(function() return pl.teamId end)
			t = okt and math.tointeger(t) or 0
			if t == 1 then n1 = n1 + 1 elseif t == 2 then n2 = n2 + 1 end
		end
	end
	return n1, n2
end

function M.cp_state(cp)
	local n1, n2 = M.inside_counts(cp)
	return {
		team = math.tointeger(cp.team) or 0, loc = cp.flagLocation, ctrl = cp.isControlled == true,
		att = math.tointeger(cp.attackingTeam) or 0, vel = cp.flagVelocity, atk = cp.isAttacked == true,
		n1 = n1, n2 = n2, cd = cp.cooldownTimer, pen1 = cp.team1CooldownPenalty, pen2 = cp.team2CooldownPenalty,
		mov = cp.isFlagMoving == true, pre = cp.isInPreRound == true, en = cp.isCaptureEnabled == true,
	}
end

function M.cp_rows(second)
	for _, cp in ipairs(M.flags()) do
		local id = ids.flag(cp)
		local okc, s = pcall(M.cp_state, cp)
		if okc then
			local sig = s.team .. ':' .. string.format('%.3f', s.loc or -1) .. ':' .. tostring(s.ctrl) .. ':' .. s.n1 ..
				':' .. s.n2 .. ':' .. tostring(s.pre)
			local active = M.cp_active[id] or M.cp_mode == 'tick'
			if active or second or M.cp_sig[id] ~= sig then
				M.cp_sig[id] = sig
				R.row('cp', { cp = id, team = s.team, loc = s.loc, ctrl = s.ctrl, att = s.att, vel = s.vel, atk = s.atk,
					n1 = s.n1, n2 = s.n2, cd = s.cd, pen1 = s.pen1, pen2 = s.pen2, mov = s.mov, pre = s.pre, en = s.en })
			end
		end
	end
end

function M.on_level()
	M.flag_list = nil
	M.tc_list = nil
	M.cp_sig = {}
	M.cp_active = {}
	M.inst = {}
	M.wf_sig = {}
end

return M

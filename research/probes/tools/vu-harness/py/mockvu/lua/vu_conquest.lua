-- Mock Conquest (DESIGN.md 13.2, 13.3): capture points (flagLocation 0-100, inside counts, capture law, flip rule,
-- contest, leave/return), HQs (capture disabled, owned), ticket counters as floats with bleed rule A or B, ticket
-- costs of spawns/deaths/removals (tcost), preround, round over.
mock = mock or {}
local M = mock
local A = M.A
local W = M.W
local S = M.S

local C = {}
M.C = C

C.flags = {}
C.counters = {}
C.tickets = { [1] = 350.0, [2] = 350.0 }
C.initial = { [1] = 350.0, [2] = 350.0 }
C.preround = false
C.round = 0
C.over = false
C.START_COUNT = 4
C.T_GET = 20.0
C.RETURN = 100 / 30   -- flagLocation per second back to the owner's state when nobody is inside

local CP_GET = {
	transform = function(o) return M.lt(M.vec3(1, 0, 0), M.vec3(0, 1, 0), M.vec3(0, 0, 1), M.a3v(o.pos)) end,
	playersInside = function(o)
		local out = {}
		for _, so in ipairs(o.inside_list or {}) do out[#out + 1] = M.px(so.pl, 'Player') end
		return out
	end,
	isInPreRound = function() return C.preround == true end,
	isAttacked = function(o) return (o.attackingTeam or 0) ~= 0 end,
	isFlagMoving = function(o) return math.abs(o.flagVelocity or 0) > 1e-9 end,
	typeInfo = function() return M.typeinfo('ServerCapturePointEntity') end,
	data = function() return nil end,
	isPendingDestruction = function() return false end,
}

local TC_GET = {
	ticketCount = function(o) return C.tickets[o.team] end,
	controlledAreaValue = function(o) return C.area(o.team) end,
	transform = function() return M.lt() end,
	typeInfo = function() return M.typeinfo('ServerTicketCounterEntity') end,
	data = function() return nil end,
	isPendingDestruction = function() return false end,
}

function C.reset(L, opts)
	opts = opts or {}
	C.flags = {}
	C.counters = {}
	local t0 = L.tickets or 350
	C.initial = { [1] = (opts.tickets1 or t0) + 0.0, [2] = (opts.tickets2 or t0) + 0.0 }
	C.tickets = { [1] = C.initial[1], [2] = C.initial[2] }
	C.preround = opts.preround == true
	C.over = false
	for _, f in ipairs(L.flags or {}) do
		local o = { __cls = 'CapturePointEntity', __etype = 'ServerCapturePointEntity', __owner = 'none', name = f.name,
			pos = A.copy(f.pos), r = f.r or 10, area = f.area or 60, hq = f.hq == true, team = f.team or 0,
			flagLocation = f.hq and 100.0 or 0.0, flagHome = 0.0, isControlled = f.hq == true, isCaptureEnabled = not f.hq,
			attackingTeam = 0, previousOwner = 0, flagVelocity = 0.0, cooldownTimer = 0.0, team1CooldownPenalty = 0.0,
			team2CooldownPenalty = 0.0, inside = {}, inside_list = {}, __get = CP_GET }
		W.add_ent(o)
		C.flags[#C.flags + 1] = o
	end
	for team = 1, 2 do
		local o = { __cls = 'TicketCounterEntity', __etype = 'ServerTicketCounterEntity', __owner = 'none', team = team,
			initialTicketCount = C.initial[team], __get = TC_GET }
		W.add_ent(o)
		C.counters[#C.counters + 1] = o
	end
end

function C.area(team)
	local a = 0
	for _, f in ipairs(C.flags) do
		if not f.hq and f.team == team then a = a + f.area end
	end
	return a
end

local function cost(team, what)
	local tc = M.laws.tcost or { spawn = 1, kill = 0, remove = 0 }
	local c = tc[what] or 0
	if (team == 1 or team == 2) and c ~= 0 and not C.preround then
		C.tickets[team] = math.max(0, C.tickets[team] - c)
	end
end

function C.on_spawn(pl) cost(pl.teamId, 'spawn') end
function C.on_death(pl, why) if why ~= 'removed' then cost(pl.teamId, 'kill') end end
function C.on_remove(pl) cost(pl.teamId, 'remove') end

-- capture time for n attackers (one half: neutral -> owned or owned -> neutral)
local function half_time(n)
	if M.laws.capture == 'linear_n' then return C.T_GET / n end
	return C.T_GET / math.min(1 + 0.34 * (n - 1), 4)
end

local function counts_players()
	local n = 0
	for _, p in ipairs(S.players) do
		if p.so and p.so.alive and (p.__owner == 'human' or not M.laws.preround) then n = n + 1 end
	end
	return n
end

function C.step(dt)
	if C.preround and counts_players() >= C.START_COUNT then
		C.preround = false
		M.mark_event('round-start')
	end
	local soldiers = S.alive_soldiers()
	for _, f in ipairs(C.flags) do
		local n = { [1] = 0, [2] = 0 }
		local now = {}
		local list = {}
		if not f.hq then
			for _, so in ipairs(soldiers) do
				local dx, dz = so.p[1] - f.pos[1], so.p[3] - f.pos[3]
				if dx * dx + dz * dz <= f.r * f.r and math.abs(so.p[2] - f.pos[2]) < 5 then
					now[so] = true
					list[#list + 1] = so
					local t = so.pl.teamId
					if t == 1 or t == 2 then n[t] = n[t] + 1 end
				end
			end
			for so in pairs(now) do
				if not f.inside[so] then
					M.dispatch('Player:EnteredCapturePoint', M.px(so.pl, 'Player'), M.px(f, 'Entity'))
				end
			end
			for so in pairs(f.inside) do
				if not now[so] and so.pl then
					M.dispatch('Player:ExitedCapturePoint', M.px(so.pl, 'Player'), M.px(f, 'Entity'))
				end
			end
		end
		f.inside = now
		f.inside_list = list
		local vel = 0
		if not f.hq and not C.preround and f.isCaptureEnabled then
			local att = 0
			if n[1] > 0 and n[2] == 0 then att = 1 elseif n[2] > 0 and n[1] == 0 then att = 2 end
			f.attackingTeam = 0
			if n[1] > 0 and n[2] > 0 then
				vel = 0 -- contested: no movement
			elseif att ~= 0 then
				local na = n[att]
				local rate = 100 / half_time(na)
				if f.team ~= 0 and f.team ~= att then
					f.attackingTeam = att
					if M.laws.flip == 'direct' then
						-- one half-time from owned to captured: the flag changes hands at once and counts up
						f.previousOwner = f.team
						f.team = att
						f.isControlled = false
						f.flagLocation = 0.0
						M.dispatch('CapturePoint:Lost', M.px(f, 'Entity'))
					else
						vel = -rate
						f.flagLocation = math.max(0, f.flagLocation - rate * dt)
						if f.flagLocation <= 0 then
							f.previousOwner = f.team
							f.team = 0
							f.isControlled = false
							M.dispatch('CapturePoint:Lost', M.px(f, 'Entity'))
						end
					end
				elseif f.team == 0 or (f.team == att and not f.isControlled) then
					f.attackingTeam = att
					if f.team == 0 then
						-- a neutral flag that another team had started to raise starts again from 0
						if f.cap_team ~= att then f.flagLocation = 0 end
						f.cap_team = att
					end
					vel = rate
					f.flagLocation = math.min(100, f.flagLocation + rate * dt)
					if f.flagLocation >= 100 then
						f.team = att
						f.isControlled = true
						M.dispatch('CapturePoint:Captured', M.px(f, 'Entity'))
					end
				end
			else
				-- nobody inside: an unfinished capture returns to its owner's state
				if not f.isControlled and f.flagLocation > 0 then
					if f.team == 0 then
						vel = -C.RETURN
						f.flagLocation = math.max(0, f.flagLocation - C.RETURN * dt)
					else
						vel = C.RETURN
						f.flagLocation = math.min(100, f.flagLocation + C.RETURN * dt)
						if f.flagLocation >= 100 then f.isControlled = true end
					end
				end
			end
		end
		f.flagVelocity = vel
	end
	-- bleed
	if not C.preround and not C.over then
		local a1, a2 = C.area(1), C.area(2)
		for team = 1, 2 do
			local own, enemy = (team == 1) and a1 or a2, (team == 1) and a2 or a1
			local rate = 0
			if M.laws.bleed == 'B' then
				local lead = (enemy - own) / 60
				if lead > 0 then rate = 7.5 * lead end
			else
				if enemy > 100 and enemy > own then rate = 15 end
			end
			if rate > 0 then C.tickets[team] = math.max(0, C.tickets[team] - rate / 60 * dt) end
		end
		for team = 1, 2 do
			if C.tickets[team] <= 0 and not C.over then
				C.over = true
				M.dispatch('Server:RoundOver', M.t, 3 - team)
				M.schedule_level('rr', 5.0)
			end
		end
	end
end

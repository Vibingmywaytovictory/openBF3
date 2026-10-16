-- M-B capture (DESIGN.md 6.8). B. Item M2.
-- One flag F (flag=<cp id>, default: the capturable flag farthest from both HQs); a pool of 5 team-1 and 4 team-2
-- bots spawned once; "bench" is a teleport (no ticket). T(n) is measured offline from the tick the n-th attacker is
-- inside (Player:EnteredCapturePoint ev or the cp inside count) to controlled. Args: flag, scale (tests).
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local ids = require('mh_ids')
local L = require('scenarios/lib')

local M = { id = 'MB', title = 'M-B capture' }

function M.needs(args) return {} end
function M.cost(args) return { s1 = 5, s2 = 4, r1 = 5, r2 = 4 } end
function M.estimate(args)
	return math.floor(1500 * math.max(0.2, math.min(1, tonumber(args.scale or '1') or 1)))
end

local function fpos(cp) return V.v(cp.transform.trans) end
local function owner(cp) return math.tointeger(cp.team) or 0 end

local function pick_flag(args)
	local flags, hqs = {}, {}
	for _, cp in ipairs(S.flags()) do
		if cp.isCaptureEnabled == false and (math.tointeger(cp.team) or 0) > 0 then
			hqs[#hqs + 1] = cp
		else
			flags[#flags + 1] = cp
		end
	end
	if args.flag then
		for _, cp in ipairs(flags) do
			if ids.flag(cp) == tonumber(args.flag) then return cp, hqs end
		end
		return nil, hqs
	end
	local best, bd = nil, -1
	for _, cp in ipairs(flags) do
		local d = math.huge
		for _, hq in ipairs(hqs) do d = math.min(d, V.dist(fpos(cp), fpos(hq))) end
		if #hqs == 0 then d = 0 end
		if d > bd then best, bd = cp, d end
	end
	return best, hqs
end

function M.run(ctx, args)
	local F, hqs = pick_flag(args)
	if F == nil then ctx:abort('no capturable flag') end
	if F.isInPreRound == true then ctx:abort('preround (L21)') end
	local fid = ids.flag(F)
	local P = fpos(F)
	local bench = {}
	for tm = 1, 2 do
		local hq = nil
		for _, h in ipairs(hqs) do if (math.tointeger(h.team) or 0) == tm then hq = h end end
		bench[tm] = hq and L.snap(fpos(hq)) or L.snap(V.add(P, { tm == 1 and 70 or -70, 0, 0 }))
	end
	local pool = { [1] = {}, [2] = {} }
	for tm, n in ipairs({ 5, 4 }) do
		for i = 1, n do
			local b = L.spawn(ctx, tm, tm == 1 and 'M16A3' or 'AK74M', L.snap(V.add(bench[tm], { 2 * i, 0, 2 })), { 0, 0, 1 }, 0)
			if b == nil then ctx:abort('pool spawn failed') end
			pool[tm][#pool[tm] + 1] = b
		end
	end
	S.cp_active[fid] = true
	local function bench_all(tm)
		for i, b in ipairs(pool[tm]) do
			if not bots.alive(b) then L.respawn(ctx, b, bench[tm], { 0, 0, 1 }, 0) end
			bots.bench(b, L.snap(V.add(bench[tm], { 2 * i, 0, 2 })), { 0, 0, 1 })
		end
	end
	local function put(tm, n)
		for i = 1, n do
			local b = pool[tm][i]
			if not bots.alive(b) then L.respawn(ctx, b, bench[tm], { 0, 0, 1 }, 0) end
			local a = 2 * math.pi * i / n
			bots.place(b, L.snap({ P[1] + 1.5 * math.cos(a), P[2], P[3] + 1.5 * math.sin(a) }), { 0, 0, 1 })
		end
	end
	local direct_seen = false
	local function unowned(cur)
		-- neutral, or (direct flip, a §9 unknown) taken by the other team but not yet controlled
		local o = owner(F)
		return o == 0 or (o ~= cur and F.isControlled ~= true)
	end
	local function neutralise(cur)
		put(3 - cur, 4)
		local ok = ctx:wait_until(function() return unowned(cur) end, 90, 'neutral')
		bench_all(3 - cur)
		if ok and owner(F) ~= 0 and not direct_seen then
			direct_seen = true
			ctx:mark('direct-flip', { x = { cp = fid, team = owner(F) } })
		end
		return ok
	end
	local function reset(to)
		-- to 0: the owner's enemy neutralises; to 2: team 2 captures. With a direct flip the flag never reads
		-- neutral: "team 2, not controlled, location 0" stands in for neutral (team 1 then flips it directly).
		local cur = owner(F)
		if cur ~= 0 and cur ~= to and not (to == 0 and direct_seen and cur == 2 and F.isControlled ~= true) then
			if not neutralise(cur) then return false end
			if to == 0 and owner(F) == 1 and not neutralise(1) then return false end
		end
		if to ~= 0 and not (owner(F) == to and F.isControlled == true) then
			put(to, 4)
			local ok = ctx:wait_until(function() return owner(F) == to and F.isControlled == true end, 90, 'own')
			bench_all(to)
			if not ok then return false end
		end
		ctx:wait(2)
		return true
	end
	local reps = L.scale(args, 3)
	ctx:config(1, 4, 'N', nil, function()
		for n = 1, 5 do
			for r = 1, reps do
				if not reset(0) then return false end
				ctx:mark('series', { cfg = 'N', i = n, k0 = C.kk(), x = { rep = r, cp = fid, n = n, from = 0 } })
				put(1, n)
				local ok = ctx:wait_until(function() return owner(F) == 1 and F.isControlled == true end, 60, 'capture')
				ctx:mark('series-end', { cfg = 'N', i = n, k0 = C.kk(), x = { rep = r, ok = ok } })
				bench_all(1)
			end
		end
		return true
	end)
	ctx:config(2, 4, 'FLIP', nil, function()
		for _, n in ipairs({ 1, 2, 4 }) do
			for r = 1, reps do
				if not reset(2) then return false end
				ctx:mark('series', { cfg = 'FLIP', i = n, k0 = C.kk(), x = { rep = r, cp = fid, n = n, from = 2 } })
				put(1, n)
				local ok = ctx:wait_until(function() return owner(F) == 1 and F.isControlled == true end, 90, 'flip')
				ctx:mark('series-end', { cfg = 'FLIP', i = n, k0 = C.kk(), x = { rep = r, ok = ok } })
				bench_all(1)
			end
		end
		return true
	end)
	ctx:config(3, 4, 'CONTEST', nil, function()
		for _, nn in ipairs({ { 1, 1 }, { 2, 1 } }) do
			if not reset(2) then return false end
			ctx:mark('contest', { cfg = 'CONTEST', k0 = C.kk(), x = { n1 = nn[1], n2 = nn[2] } })
			put(1, nn[1])
			put(2, nn[2])
			ctx:wait(60)
			ctx:mark('contest-end', { cfg = 'CONTEST', k0 = C.kk() })
			bench_all(1)
			bench_all(2)
		end
		return true
	end)
	ctx:config(4, 4, 'LEAVE', nil, function()
		for r = 1, reps do
			if not reset(0) then return false end
			put(1, 1)
			ctx:wait_until(function() return (F.flagLocation or 0) >= 50 or (owner(F) == 1) end, 60, 'half')
			bench_all(1)
			ctx:mark('leave', { cfg = 'LEAVE', i = r, k0 = C.kk() })
			ctx:wait(40)
		end
		return true
	end)
	reset(0)
end

function M.cleanup(ctx) end

return M

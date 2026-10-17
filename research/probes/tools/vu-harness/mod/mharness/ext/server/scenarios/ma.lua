-- M-A ticket bleed (DESIGN.md 6.7). B, fully automatic. Item M1.
-- Flags and HQs from the ServerCapturePointEntity iterator (HQ = capture disabled with an owner) [live:L15]; a pool of
-- 2 bots per team spawned once and moved (teleported) between flags and the team's bench; HOLD windows are marked
-- with k0..k1 and the ownership vector own (only these enter the rate fit). Adaptive mirroring keeps both teams above
-- the guard. Args: hold=<s> (default 150), scale (tests).
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local ids = require('mh_ids')
local L = require('scenarios/lib')

local M = { id = 'MA', title = 'M-A ticket bleed' }

M.PLANS = {
	MP_Subway = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 2, 1 }, { 1, 1 } },
	MP_001 = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 }, { 4, 1 }, { 3, 2 }, { 2, 2 } },
}
M.RATE = 15 / 60  -- tickets per second under rule A (P section 7.3)

function M.needs(args) return { levels = { 'MP_Subway', 'MP_001' } } end

local function hold_s(args)
	return tonumber(args.hold or '150') or 150
end

-- worst-case bleed of the plan for the forecast: rule B up to twice rule A for every bleeding state.
function M.cost(args, runner)
	local lvl = runner and runner.level or 'MP_Subway'
	local plan = (lvl and lvl:find('MP_001')) and M.PLANS.MP_001 or M.PLANS.MP_Subway
	local b = 0
	for _, st in ipairs(plan) do
		if st[1] ~= st[2] then b = b + 2 * M.RATE * hold_s(args) end
	end
	return { s1 = 2, s2 = 2, r1 = 2, r2 = 2, b2 = b }
end

function M.estimate(args)
	return 9 * (hold_s(args) + 60) + 60
end

local function flag_pos(cp)
	return V.v(cp.transform.trans)
end

local function classify()
	local flags, hqs = {}, {}
	for _, cp in ipairs(S.flags()) do
		local en = cp.isCaptureEnabled
		local tm = math.tointeger(cp.team) or 0
		if en == false and tm > 0 then hqs[tm] = cp else flags[#flags + 1] = cp end
	end
	table.sort(flags, function(a, b) return ids.flag(a) < ids.flag(b) end)
	return flags, hqs
end

local function owner(cp)
	return math.tointeger(cp.team) or 0
end

-- move(ctx, pool, team, cp, want): place the team's pool at the flag until want(cp) holds; then bench.
local function move(ctx, pool, team, cp, want, bench, timeout)
	local p = flag_pos(cp)
	S.cp_active[ids.flag(cp)] = true
	for i, b in ipairs(pool[team]) do
		if not bots.alive(b) then L.respawn(ctx, b, bench[team], { 0, 0, 1 }, 0) end
		bots.place(b, L.snap({ p[1] + (i - 1.5) * 1.5, p[2], p[3] + 1 }), { 0, 0, 1 })
	end
	local ok = ctx:wait_until(function() return want(cp) end, timeout, 'flag')
	if not ok then
		for i, b in ipairs(pool[team]) do bots.place(b, L.snap({ p[1] + (i - 1.5) * 3, p[2], p[3] - 3 }), { 0, 0, 1 }) end
		ok = ctx:wait_until(function() return want(cp) end, timeout, 'flag-retry')
	end
	for i, b in ipairs(pool[team]) do bots.bench(b, L.snap(V.add(bench[team], { 2 * i, 0, 0 })), { 0, 0, 1 }) end
	S.cp_active[ids.flag(cp)] = nil
	return ok
end

local function set_flag(ctx, pool, cp, target, bench)
	local cur = owner(cp)
	if cur == target and (target == 0 or cp.isControlled == true) then return true end
	if cur ~= 0 then
		local enemy = 3 - cur
		if not move(ctx, pool, enemy, cp, function(c) return owner(c) == 0 end, bench, 90) then return false end
	end
	if target ~= 0 then
		return move(ctx, pool, target, cp, function(c) return owner(c) == target and c.isControlled == true end, bench, 90)
	end
	return true
end

function M.run(ctx, args)
	local lvl = SharedUtils:GetLevelName() or ''
	local key = lvl:find('MP_001') and 'MP_001' or (lvl:find('MP_Subway') and 'MP_Subway' or nil)
	if key == nil then ctx:abort('M-A needs MP_Subway or MP_001') end
	local flags, hqs = classify()
	if #flags == 0 then ctx:abort('no capture points (L15)') end
	for _, cp in ipairs(flags) do
		if cp.isInPreRound == true then ctx:abort('preround (L21)') end
	end
	local bench = {}
	for tm = 1, 2 do
		bench[tm] = hqs[tm] and L.snap(flag_pos(hqs[tm])) or L.snap(V.add(flag_pos(flags[1]), { tm == 1 and 60 or -60, 0, 0 }))
	end
	local pool = { [1] = {}, [2] = {} }
	for tm = 1, 2 do
		for i = 1, 2 do
			local b = L.spawn(ctx, tm, tm == 1 and 'M16A3' or 'AK74M', L.snap(V.add(bench[tm], { 2 * i, 0, 0 })), { 0, 0, 1 }, 0)
			if b == nil then ctx:abort('pool spawn failed') end
			pool[tm][#pool[tm] + 1] = b
		end
	end
	S.tk_mode = 'tick'
	local plan = {}
	for _, st in ipairs(M.PLANS[key]) do
		if st[1] + st[2] <= #flags then plan[#plan + 1] = st end
	end
	local hs = hold_s(args)
	local guard = require('mh_runner').GUARD
	for i, st in ipairs(plan) do
		local a, b = st[1], st[2]
		local bleeding = (a > b) and 2 or ((b > a) and 1 or 0)
		if bleeding ~= 0 then
			local worst = 2 * M.RATE * hs + 10
			local t = S.tickets(bleeding) or 0
			local other = S.tickets(3 - bleeding) or 0
			if t - worst < guard + 10 then
				if other - worst >= guard + 10 then
					a, b = b, a
					bleeding = 3 - bleeding
					ctx:mark('mirror', { i = i, x = { a = a, b = b } })
				else
					ctx:mark('plan-end', { i = i, x = { why = 'tickets' } })
					break
				end
			end
		end
		local label = string.format('%d-%d', a, b)
		ctx:config(i, #plan, label, { a = a, b = b }, function()
			-- CHANGE: first a flags to team 1, next b flags to team 2, the rest neutral (flag id order)
			local own = {}
			for f, cp in ipairs(flags) do
				local target = (f <= a) and 1 or ((f <= a + b) and 2 or 0)
				own[f] = target
				if not set_flag(ctx, pool, cp, target, bench) then return false end
			end
			-- re-check every flag (a later move can disturb an earlier one)
			for f, cp in ipairs(flags) do
				if owner(cp) ~= own[f] then
					if not set_flag(ctx, pool, cp, own[f], bench) then return false end
				end
			end
			local k0 = C.kk()
			ctx:wait(hs)
			ctx:mark('hold', { cfg = label, i = i, k0 = k0, k1 = C.kk(), own = own })
			return true
		end)
	end
end

function M.cleanup(ctx)
	S.tk_mode = '1hz'
end

-- M-A bleeds by design; the runner's 10 s guard still applies (DESIGN 3.8).
return M

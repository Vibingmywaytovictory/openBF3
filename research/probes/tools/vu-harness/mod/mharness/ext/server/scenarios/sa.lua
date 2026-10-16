-- S-A health and regeneration (DESIGN.md 6.6). B or U. Items S1, S2, S4.
-- Args: mode=U|B, scale (tests), resp=1 (U: optional suicide/respawn timer). Suppression evidence per rep:
-- Player:SuppressedEnemy for the target (server/event/player_suppressedenemy.md) or a change of its gs smf.
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local ids = require('mh_ids')
local EV = require('mh_events')
local L = require('scenarios/lib')

local M = { id = 'SA', title = 'S-A health' }

M.supp = {}  -- victim sid -> count of Player:SuppressedEnemy
EV.on('Player:SuppressedEnemy', function(player, enemy)
	local sid = ids.player(enemy)
	if sid then M.supp[sid] = (M.supp[sid] or 0) + 1 end
end)

M.DELAYS = { 0.5, 1.0, 1.5, 2.5 }

function M.needs(args)
	local n = { sites = { 'range' } }
	if (args.mode or 'U') == 'B' then
		n.aimcal = true
		n.damage = true
	end
	return n
end

function M.cost(args)
	local imm = #M.DELAYS * L.scale(args, 3)
	if (args.mode or 'U') == 'B' then return { s1 = 1, r1 = 1, s2 = 2 + imm, d2 = imm, r2 = 1 } end
	return { s2 = 2 + imm + 10, d2 = imm + 10, r2 = 1 }
end

function M.estimate(args)
	local s = math.min(1, tonumber(args.scale or '1') or 1)
	return ((args.mode or 'U') == 'B' and 540 or 720) * s
end

local function one_hit(ctx, shooter, T, user_sid, label, n, of)
	local k0 = C.gk
	local giver = shooter and shooter.sid or user_sid
	if shooter then
		local tries = 0
		while (L.dmg_pairs_since(k0, T.sid, giver)) == 0 and tries < 10 do
			bots.aim_at(shooter, L.chest(T))
			ctx:wait(0.2)
			L.fire1(ctx, shooter)
			tries = tries + 1
			ctx:wait(0.5)
		end
	else
		ctx:prompt('shoot', { n = n, of = of, zone = 'chest', d = 8, tid = T.sid },
			string.format('%s: one chest shot at the target (%d/%d), then wait', label, n, of))
		ctx:wait_until(function() return (L.dmg_pairs_since(k0, T.sid, giver)) > 0 end, 60, 'hit')
	end
	return (L.dmg_pairs_since(k0, T.sid, giver)) > 0
end

local function smf_of(sid)
	local g = S.last_gs[sid]
	return g and g.smf or nil
end

function M.run(ctx, args)
	local rng = ctx:need_site('range')
	if rng == nil then ctx:abort('no range site') end
	local mode = args.mode or 'U'
	local O, u = rng.pos, rng.dir
	local side = V.yrot(u, math.pi / 2)
	local face_t = V.scale(u, -1)
	local tpos = L.snap(V.add(O, V.scale(u, 8)))
	local shooter, user_sid = nil, nil
	if mode == 'B' then
		shooter = L.spawn(ctx, 1, 'M16A3', O, u, 0)
		if shooter == nil then ctx:abort('shooter spawn failed') end
		bots.zoom(shooter, true)
	else
		user_sid = ids.player(ctx.owner)
		S.watch(user_sid, { hp = true })
		H.human_watch[user_sid] = true
		ctx:prompt('position', { n = 1, of = 1 }, 'stand on the range start with the M16A3 (US); the target is at 8 m')
		ctx:wait(5)
	end
	local T = L.spawn(ctx, 2, 'AK74M', tpos, face_t, 0)
	if T == nil then ctx:abort('target spawn failed') end
	S.watch(T.sid, { hp = true, gs = true })
	local reps = L.scale(args, 5)
	ctx:config(1, 3, 'regen', nil, function()
		for r = 1, reps do
			if not bots.alive(T) or bots.health(T) < 100 then
				if not bots.alive(T) then L.respawn(ctx, T, tpos, face_t, 0) end
				ctx:wait_until(function() return bots.health(T) >= 100 end, 30, 'full')
			end
			ctx:mark('rep', { cfg = 'regen', i = r, k0 = C.kk() })
			if one_hit(ctx, shooter, T, user_sid, 'regen', r, reps) then
				ctx:mark('hit', { cfg = 'regen', i = r, k0 = C.kk() })
				ctx:wait(40)
			end
		end
		return true
	end)
	ctx:config(2, 3, 'supp', nil, function()
		local sreps = L.scale(args, 3)
		for r = 1, sreps do
			if not bots.alive(T) then L.respawn(ctx, T, tpos, face_t, 0) end
			ctx:wait_until(function() return bots.health(T) >= 100 end, 30, 'full')
			ctx:mark('rep', { cfg = 'supp', i = r, k0 = C.kk() })
			local ev0 = M.supp[T.sid] or 0
			local smf0 = smf_of(T.sid)
			if one_hit(ctx, shooter, T, user_sid, 'supp', r, sreps) then
				ctx:mark('hit', { cfg = 'supp', i = r, k0 = C.kk() })
				ctx:wait(4)
				local spot = V.add(L.chest(T) or tpos, V.scale(side, 2.0))
				ctx:mark('spray', { cfg = 'supp', i = r, k0 = C.kk() })
				if shooter then
					bots.aim_at(shooter, spot)
					ctx:wait(0.2)
					L.fire_n(ctx, shooter, 25, 800)
				else
					ctx:prompt('spray', { n = r, of = sreps, d = 2 },
						'spray a burst about 2 m to the side of the target now, do not hit it (one magazine)')
					ctx:wait(3)
				end
				ctx:wait(34)
				local ev1 = M.supp[T.sid] or 0
				local smf1 = smf_of(T.sid)
				local evidence = ev1 > ev0 or (smf0 ~= nil and smf1 ~= nil and smf1 ~= smf0)
				ctx:mark(evidence and 'supp-seen' or 'supp-none', { cfg = 'supp', i = r, x = { ev = ev1 - ev0 } })
			end
		end
		return true
	end)
	ctx:config(3, 3, 'immune', nil, function()
		local n = L.scale(args, 3)
		if shooter then
			for _, delay in ipairs(M.DELAYS) do
				for r = 1, n do
					if bots.alive(T) then bots.kill(T) end
					ctx:wait(1.0)
					if not L.respawn(ctx, T, tpos, face_t, 0) then return false end
					local sk = C.kk()
					bots.aim_at(shooter, L.chest(T))
					ctx:mark('immune', { cfg = 'immune', i = r, k0 = sk, x = { delay = delay } })
					ctx:wait(math.max(0.05, delay - 0.3))
					L.fire1(ctx, shooter)
					ctx:wait(1.0)
				end
			end
		else
			ctx:prompt('immune', { n = 1, of = 1, wait = '60s' },
				"keep firing at the target's spawn point for 60 s while it respawns every 6 s")
			for r = 1, 10 do
				if bots.alive(T) then bots.kill(T) end
				ctx:wait(0.5)
				if not L.respawn(ctx, T, tpos, face_t, 0) then return false end
				ctx:mark('immune', { cfg = 'immune', i = r, k0 = C.kk(), x = { delay = 0 } })
				ctx:wait(5.5)
			end
		end
		return true
	end)
	if mode == 'U' and args.resp == '1' then
		ctx:prompt('suicide', { n = 1, of = 1 }, 'optional: suicide from the menu now and respawn when allowed')
		ctx:wait(30)
	end
	if user_sid then
		H.human_watch[user_sid] = nil
		S.unwatch(user_sid)
	end
end

function M.cleanup(ctx)
	H.human_watch = {}
end

return M

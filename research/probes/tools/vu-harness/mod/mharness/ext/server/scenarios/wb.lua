-- W-B falloff (DESIGN.md 6.5). U (the user shoots) or B (bots, if P0.9 said bot damage YES). Items W5, W6.
-- Args: mode=U|B, w=M16A3,M98B[,AK74M,M9,870MCS], zones=chest[,head], heal=regen|respawn, pass=0|1, scale (tests).
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local ids = require('mh_ids')
local L = require('scenarios/lib')
local W = require('scenarios/wa_cfg')

local M = { id = 'WB', title = 'W-B falloff' }

M.D = { 5, 8, 10, 15, 20, 25, 30, 40, 50, 60, 75, 100, 125, 150, 175 }

local function list(s, default)
	local out = {}
	for tok in (s or default):gmatch('[^,]+') do out[#out + 1] = tok end
	return out
end

function M.weapons(args)
	local out = {}
	for _, w in ipairs(list(args.w, 'M16A3,M98B')) do
		local key = W.canonical(w)
		if key then out[#out + 1] = key end
	end
	return out
end

function M.zones(args)
	local mode = args.mode or 'U'
	return list(args.zones, mode == 'B' and 'chest,head' or 'chest')
end

function M.distances(w, clear)
	local out = {}
	for _, d in ipairs(M.D) do
		if d <= (clear or 200) - 2 and (w ~= '870MCS' or d <= 30) then out[#out + 1] = d end
	end
	return out
end

function M.needs(args)
	local n = { sites = { 'range' } }
	if (args.mode or 'U') == 'B' then
		n.aimcal = true
		n.botdmg = true
	end
	return n
end

local function hits_per(args) return L.scale(args, 3) end

function M.cost(args)
	local c = { s1 = 0, d1 = 0, r1 = 0, s2 = 0, d2 = 0, r2 = 0 }
	local mode = args.mode or 'U'
	for _, w in ipairs(M.weapons(args)) do
		local ts = W.teams[w] or 1
		local tt = 3 - ts
		local deaths = 0
		for _, z in ipairs(M.zones(args)) do
			local hits = #M.distances(w, 200) * hits_per(args)
			if z == 'head' or w == 'M98B' or w == '870MCS' then
				deaths = deaths + hits
			elseif mode == 'U' and (args.heal or 'respawn') == 'respawn' then
				deaths = deaths + math.ceil(hits / 3)
			end
		end
		local key_s, key_t = 's' .. tt, 'd' .. tt
		c[key_s] = c[key_s] + 2 + deaths
		c[key_t] = c[key_t] + deaths
		c['r' .. tt] = c['r' .. tt] + 2
		if mode == 'B' then
			c['s' .. ts] = c['s' .. ts] + 1
			c['r' .. ts] = c['r' .. ts] + 1
		end
	end
	return c
end

function M.estimate(args)
	local per = (args.mode or 'U') == 'B' and 600 or 900
	local s = tonumber(args.scale or '1') or 1
	return #M.weapons(args) * #M.zones(args) * per * math.min(1, s)
end

-- heal(ctx, T, w, d, zone, policy, tpos, face): respawn on death; heal before a hit that could kill.
local function heal(ctx, T, w, d, zone, policy, tpos, face)
	if not bots.alive(T) then
		ctx:mark('heal', { x = { how = 'respawn', why = 'dead' } })
		ctx:wait(1.0)
		return L.respawn(ctx, T, tpos, face, 0)
	end
	local hp = bots.health(T)
	local nxt = W.expected_damage(w, d, zone)
	if hp >= 100 or hp > 1.1 * nxt then return true end
	if policy == 'regen' then
		ctx:mark('heal', { x = { how = 'regen', hp = hp } })
		return ctx:wait_until(function() return bots.health(T) >= 100 end, 30, 'regen')
	end
	ctx:mark('heal', { x = { how = 'respawn', hp = hp } })
	bots.kill(T)
	ctx:wait(1.0)
	return L.respawn(ctx, T, tpos, face, 0)
end

local function user_ready(ctx, w)
	ctx.ok_flag = false
	local wid = ids.weapon((W.unlocks[w] or {})[1])
	ctx:prompt('equip', { n = 1, of = 1, wid = wid }, string.format('equip %s (team %s, no attachments), stand on the ' ..
		'range start, then !mh ok', w, (W.teams[w] or 1) == 1 and 'US' or 'RU'))
	return ctx:wait_until(function() return ctx.ok_flag end, 300, 'equip')
end

local function shoot_at(ctx, shooter, T, user_sid, zone, n_hits, max_shots, w, d, policy, tpos, face, tag)
	local got, shots = 0, 0
	local k_start = C.gk
	while got < n_hits do
		if C.gk - k_start > 120 * C.hzv then
			ctx:mark('skip-distance', { x = { d = d, zone = zone } })
			return false
		end
		if not heal(ctx, T, w, d, zone, policy, tpos, face) then return false end
		local k0 = C.gk
		local giver = shooter and shooter.sid or user_sid
		if shooter then
			if shots >= max_shots then
				ctx:mark('skip-distance', { x = { d = d, zone = zone, why = 'shots' } })
				return false
			end
			bots.aim_at(shooter, L.chest(T, zone))
			ctx:wait(0.2)
			L.fire1(ctx, shooter)
			shots = shots + 1
			ctx:wait(0.6)
		else
			ctx:prompt('shoot', { n = got + 1, of = n_hits, zone = zone, d = d, tid = T.sid },
				string.format('%s%s at %d m (%d/%d)', tag or '', zone, d, got + 1, n_hits))
			ctx:wait_until(function() return (L.dmg_pairs_since(k0, T.sid, giver)) > 0 end, 120, 'hit')
		end
		local n = L.dmg_pairs_since(k0, T.sid, giver)
		got = got + n
	end
	return true
end

local function run_weapon(ctx, args, w, rng, mode)
	local O, u = rng.pos, rng.dir
	local side = V.yrot(u, math.pi / 2)
	local face_t = V.scale(u, -1)
	local ts = W.teams[w] or 1
	local tt = 3 - ts
	local shooter, user_sid = nil, nil
	if mode == 'B' then
		shooter = L.spawn(ctx, ts, w, O, u, 0)
		if shooter == nil then return false end
		ctx.shooter = shooter
		bots.zoom(shooter, true)
	else
		if not user_ready(ctx, w) then return false end
		user_sid = ids.player(ctx.owner)
		S.watch(user_sid, { hp = true })
		H.human_watch[user_sid] = true
	end
	local bench = L.snap(V.add(V.add(O, V.scale(side, 6)), V.scale(u, -3)))
	local T = L.spawn(ctx, tt, tt == 1 and 'M16A3' or 'AK74M', bench, face_t, 0)
	if T == nil then return false end
	ctx.target = T
	S.watch(T.sid, { hp = true })
	local policy = args.heal or (mode == 'B' and 'regen' or 'respawn')
	local D = M.distances(w, rng.clear)
	local skipped = 0
	for _, zone in ipairs(M.zones(args)) do
		ctx:mark('part', { cfg = 'wb-' .. w, x = { p = zone } })
		for _, d in ipairs(D) do
			local tpos = L.snap(V.add(O, V.scale(u, d)))
			if not bots.alive(T) then L.respawn(ctx, T, tpos, face_t, 0) end
			bots.place(T, tpos, face_t)
			ctx:mark('dist', { cfg = 'wb-' .. w, x = { d = d, zone = zone, mode = mode } })
			ctx:wait(1.0)
			if not shoot_at(ctx, shooter, T, user_sid, zone, hits_per(args), 30, w, d, policy, tpos, face_t) then
				skipped = skipped + 1
			end
		end
	end
	-- PASS: T1 at 20 m, T2 at 21 m in line; 5 chest shots at T1 (W6)
	if args.pass ~= '0' then
		local p1 = L.snap(V.add(O, V.scale(u, 20)))
		local p2 = L.snap(V.add(O, V.scale(u, 21)))
		local T2 = L.spawn(ctx, tt, tt == 1 and 'M16A3' or 'AK74M', p2, face_t, 0)
		if T2 then
			S.watch(T2.sid, { hp = true })
			if not bots.alive(T) then L.respawn(ctx, T, p1, face_t, 0) end
			bots.place(T, p1, face_t)
			ctx:mark('pass', { cfg = 'wb-' .. w, x = { t1 = T.sid, t2 = T2.sid } })
			ctx:wait(1.0)
			shoot_at(ctx, shooter, T, user_sid, 'chest', L.scale(args, 5), 40, w, 20, policy, p1, face_t, 'pass-through: ')
			ctx:mark('pass-end', { cfg = 'wb-' .. w })
			S.unwatch(T2.sid)
			ctx:drop_bot(T2)
		end
	end
	if user_sid then
		S.unwatch(user_sid)
		H.human_watch[user_sid] = nil
	end
	S.unwatch(T.sid)
	ctx:drop_bot(T)
	ctx.target = nil
	if shooter then
		ctx:drop_bot(shooter)
		ctx.shooter = nil
	end
	return skipped < #D
end

function M.run(ctx, args)
	local rng = ctx:need_site('range')
	if rng == nil then ctx:abort('no range site') end
	local mode = args.mode or 'U'
	local ws = M.weapons(args)
	if #ws == 0 then ctx:abort('no valid weapon in w=') end
	ctx.cleanup_cfg = function(cx)
		for _, key in ipairs({ 'shooter', 'target' }) do
			local b = cx[key]
			if b then
				S.unwatch(b.sid)
				cx:drop_bot(b)
				cx[key] = nil
			end
		end
	end
	for i, w in ipairs(ws) do
		ctx:config(i, #ws, 'wb-' .. w, { w = w, mode = mode, clear = rng.clear },
			function() return run_weapon(ctx, args, w, rng, mode) end)
	end
end

function M.cleanup(ctx)
	H.human_watch = {}
end

return M

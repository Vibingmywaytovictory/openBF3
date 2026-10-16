-- W-A fixed wall and W-A60 (DESIGN.md 6.2, 6.3). B (bots only). Items W1-W4, T1.
-- run(ctx, args) is a coroutine body resumed once per tick (mh_runner). Args: tier=1|2|rest|all|60, cfg=<i>,
-- from=<i>, parts=a,b,c,d,e, scale=<0..1> (tests), triples=1 (L11: gs.rec unusable).
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local L = require('scenarios/lib')
local W = require('scenarios/wa_cfg')

local M = { id = 'WA', title = 'W-A fixed wall' }

function M.list(args)
	local tier = args.tier or '1'
	local src = W.configs
	local lo, hi = 1, #src
	if tier == '60' then
		src = W.tier60
		lo, hi = 1, #src
	elseif tier == '1' then lo, hi = 1, 2
	elseif tier == '2' then lo, hi = 3, 6
	elseif tier == 'rest' then lo, hi = 3, #src
	end
	local out = {}
	for i = lo, hi do out[#out + 1] = src[i] end
	if args.cfg then
		local c = out[tonumber(args.cfg) or 0]
		out = c and { c } or {}
	end
	return out
end

local function parts_of(args, proto)
	local want = {}
	for p in (args.parts or 'a,b,c,d,e'):gmatch('[a-e]') do want[p] = true end
	if proto == 'bolt' then
		want.b, want.d, want.e = false, false, false
	end
	return want
end

local function rpm(w) return W.rpm[w] or 600 end

function M.label(c)
	return string.format('%s-%s-%s', c.w, c.stance, c.zoom and 'ads' or 'hip')
end

local function proto_time(c, args)
	local p = W.protocols[c.proto]
	local want = parts_of(args, c.proto)
	local sh = 60 / rpm(c.w)
	local t = 20
	if want.a then t = t + L.scale(args, p.a) * (1.6 + sh) end
	if want.b and p.b then t = t + L.scale(args, p.b) * (1.5 + 5 * sh) end
	if want.c then
		for _, g in ipairs(p.gaps) do t = t + L.scale(args, p.c) * (g + 2.7) end
	end
	if want.d and p.d then t = t + L.scale(args, p.d) * (31 * sh + 6) end
	if want.e and p.e then t = t + L.scale(args, p.e) * (15 * sh + 4) end
	return t
end

local function proto_shots(c, args)
	local p = W.protocols[c.proto]
	local want = parts_of(args, c.proto)
	local n = 0
	if want.a then n = n + L.scale(args, p.a) end
	if want.b and p.b then n = n + 5 * L.scale(args, p.b) end
	if want.c then n = n + 2 * #p.gaps * L.scale(args, p.c) end
	if want.d and p.d then n = n + 31 * L.scale(args, p.d) end
	if want.e and p.e then n = n + 15 * L.scale(args, p.e) end
	return n
end

function M.estimate(args)
	local t = 0
	local list = M.list(args)
	local from = tonumber(args.from or '1') or 1
	for i, c in ipairs(list) do if i >= from then t = t + proto_time(c, args) end end
	return t
end

function M.cost(args)
	local c1 = { s1 = 0, d1 = 0, r1 = 0, s2 = 0, d2 = 0, r2 = 0 }
	local from = tonumber(args.from or '1') or 1
	for i, c in ipairs(M.list(args)) do
		if i >= from then
			local team = W.team[c.w] or 1
			local resp = math.ceil(proto_shots(c, args) / (W.ammo_total[c.w] or 150))
			if team == 1 then
				c1.s1, c1.d1, c1.r1 = c1.s1 + 1 + resp, c1.d1 + resp, c1.r1 + 1
			else
				c1.s2, c1.d2, c1.r2 = c1.s2 + 1 + resp, c1.d2 + resp, c1.r2 + 1
			end
		end
	end
	return c1
end

function M.needs(args)
	return { sites = { 'wall' }, aimcal = true }
end

local function unit_mark(ctx, label, p, i, x)
	x = x or {}
	x.p = p
	ctx:mark('u', { cfg = label, i = i, x = x })
end

local function run_cfg(ctx, c, args, wall, label)
	local proto = W.protocols[c.proto]
	local want = parts_of(args, c.proto)
	local pose = L.POSES[c.stance] or 0
	local d = c.zoom and W.distance.ads or W.distance.hip
	local face = V.scale(wall.n, -1)
	local function stand_at(dd) return L.snap(V.add(wall.pos, V.scale(wall.n, dd))) end
	local pos = stand_at(d)
	local team = W.team[c.w] or 1
	local bot = L.spawn(ctx, team, c.w, pos, face, pose)
	if bot == nil then return false end
	ctx.shooter = bot
	S.watch(bot.sid, { hp = true, gs = true, wf = true })
	bots.pose(bot, pose)
	bots.zoom(bot, c.zoom)
	ctx:wait(1.0)
	bots.aim_at(bot, wall.pos)
	local mag = L.ammo(bot)
	if mag <= 0 then mag = 30 end
	L.wait_clear(ctx, bots.eye(bot), wall.pos)
	-- SETTLE: da stable at dmin for 10 ticks (<= 5 s)
	local stable = 0
	ctx:wait_until(function()
		local g = S.last_gs[bot.sid]
		if g and g.da and g.dmin and math.abs(g.da - g.dmin) < 1e-6 then stable = stable + 1 else stable = 0 end
		return stable >= 10
	end, 5, 'settle')
	local function keep_alive()
		if not bots.alive(bot) then
			if not L.respawn(ctx, bot, pos, face, pose) then return false end
			bots.pose(bot, pose)
			bots.zoom(bot, c.zoom)
			ctx:wait(1.0)
			bots.aim_at(bot, wall.pos)
		end
		return true
	end
	local function ammo_for(n)
		if not keep_alive() then return false end
		local okr = L.ensure_ammo(ctx, bot, n, mag, pos, face, pose)
		if not bots.alive(bot) or not okr then return keep_alive() end
		bots.zoom(bot, c.zoom)
		bots.aim_at(bot, wall.pos)
		return true
	end
	local auto_ok = true
	local need_auto = (want.b and proto.b) or (want.d and proto.d) or (want.e and proto.e) or want.c
	if need_auto and (W.fire[c.w] or 2) == 2 then
		auto_ok = L.ensure_fire(ctx, bot, 2)
		if not auto_ok then
			ctx:mark('no-auto', { cfg = label })
			ctx:say(label .. ': fire mode not auto, parts a and c only')
		end
	end
	local r = rpm(c.w)
	local skipped_units = 0
	local function unit_ok(ok)
		if not ok then
			skipped_units = skipped_units + 1
			if skipped_units > 20 then error({ mh = 'skip', why = 'fire timeouts' }, 0) end
		end
	end
	-- A_SINGLE
	if want.a then
		ctx:mark('part', { cfg = label, x = { p = 'a' } })
		for i = 1, L.scale(args, proto.a) do
			if not ammo_for(1) then return false end
			unit_mark(ctx, label, 'a', i)
			unit_ok(L.fire1(ctx, bot))
			ctx:wait(1.5)
		end
	end
	-- B_BURST
	if want.b and proto.b and auto_ok then
		ctx:mark('part', { cfg = label, x = { p = 'b' } })
		for i = 1, L.scale(args, proto.b) do
			if not ammo_for(5) then return false end
			unit_mark(ctx, label, 'b', i)
			unit_ok((L.fire_n(ctx, bot, 5, r)))
			ctx:wait(1.5)
		end
	end
	-- C_PAIRS (triples with args.triples)
	if want.c then
		ctx:mark('part', { cfg = label, x = { p = 'c' } })
		local i = 0
		for _, g in ipairs(proto.gaps) do
			if g <= 0.08 and not auto_ok then
				ctx:mark('no-auto-gap', { cfg = label, x = { g = g } })
			else
				for _ = 1, L.scale(args, proto.c) do
					i = i + 1
					local per = args.triples and 3 or 2
					if not ammo_for(per) then return false end
					unit_mark(ctx, label, 'c', i, { g = g })
					if g <= 0.08 then
						unit_ok((L.fire_n(ctx, bot, 2, r)))
					else
						unit_ok(L.fire1(ctx, bot))
						ctx:wait(g)
						unit_ok(L.fire1(ctx, bot))
					end
					if args.triples then
						ctx:wait(0.1)
						unit_ok(L.fire1(ctx, bot))
					end
					ctx:wait(2.5)
				end
			end
		end
	end
	-- D_MAG (ADS at 12 m so the climb stays on the wall)
	if want.d and proto.d and auto_ok then
		ctx:mark('part', { cfg = label, x = { p = 'd' } })
		local dpos = c.zoom and stand_at(W.distance.ads_mag) or pos
		bots.place(bot, dpos, face)
		ctx:wait(0.5)
		bots.aim_at(bot, wall.pos)
		for i = 1, L.scale(args, proto.d) do
			if not ammo_for(mag) then return false end
			unit_mark(ctx, label, 'd', i)
			unit_ok((L.fire_n(ctx, bot, mag, r)))
			L.reload(ctx, bot, mag)
			ctx:wait(3.0)
		end
		bots.place(bot, pos, face)
		ctx:wait(0.5)
		bots.aim_at(bot, wall.pos)
	end
	-- E_RECOV: bursts of 10 then 20, per-tick gs (always watched)
	if want.e and proto.e and auto_ok then
		ctx:mark('part', { cfg = label, x = { p = 'e' } })
		local ne = L.scale(args, proto.e)
		for i = 1, 2 * ne do
			local n = i <= ne and 10 or 20
			if not ammo_for(n) then return false end
			unit_mark(ctx, label, 'e', i, { nb = n })
			unit_ok((L.fire_n(ctx, bot, n, r)))
			ctx:wait(4.0)
		end
	end
	S.unwatch(bot.sid)
	ctx:drop_bot(bot)
	ctx.shooter = nil
	return true
end

function M.run(ctx, args)
	local wall = ctx:need_site('wall')
	if wall == nil then ctx:abort('no wall site') end
	if bots.cal == nil then ctx:abort('no aim calibration (P0.5)') end
	local list = M.list(args)
	local from = tonumber(args.from or '1') or 1
	ctx.cleanup_cfg = function(cx)
		if cx.shooter then
			S.unwatch(cx.shooter.sid)
			cx:drop_bot(cx.shooter)
			cx.shooter = nil
		end
	end
	for i, c in ipairs(list) do
		if i >= from then
			local label = M.label(c)
			ctx:config(i, #list, label, { w = c.w, stance = c.stance, zoom = c.zoom, proto = c.proto, sway = c.sway == true },
				function() return run_cfg(ctx, c, args, wall, label) end)
		end
	end
end

function M.cleanup(ctx) end

return M

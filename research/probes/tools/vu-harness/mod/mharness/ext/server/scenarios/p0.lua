-- Phase 0 shakedown (DESIGN.md 6.1). One run per step: '!mh p0 <step>' (round level tick aim bots fire damage human
-- log report all). Results go into 'p0' rows and mh_runner.p0 (queue validation reads them).
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local sites = require('mh_sites')
local ids = require('mh_ids')
local meta = require('mh_meta')
local L = require('scenarios/lib')

local M = { id = 'P0', title = 'Phase 0' }

local function runner() return require('mh_runner') end

local function result(ctx, step, ok, val, text)
	R.row('p0', { step = step, ok = ok == true, val = val })
	local rp = runner().p0
	rp.steps = rp.steps or {}
	rp.steps[step] = { ok = ok == true, text = text }
	if text then ctx:say(text) end
end

-- front(ctx): a point and a horizontal direction in front of the user (camera if known, else the body forward).
local function front(ctx)
	local p = L.owner_pos(ctx)
	if p == nil then return nil end
	local sid = ids.player(ctx.owner)
	local cam = sites.cams[sid]
	local f = cam and V.horiz(cam.fwd) or nil
	if f == nil or V.len(f) < 0.5 then
		local ok, fw = pcall(function() return V.v(ctx.owner.soldier.worldTransform.forward) end)
		f = ok and V.horiz(fw) or { 0, 0, 1 }
	end
	return p, f
end

-- benches: team -> position (the team's HQ capture point if found, else beside the user).
function M.benches(ctx)
	local out = {}
	for _, cp in ipairs(S.flags()) do
		local ok, en, tm, pos = pcall(function() return cp.isCaptureEnabled, math.tointeger(cp.team), V.v(cp.transform.trans) end)
		if ok and en == false and tm and tm > 0 and out[tm] == nil then out[tm] = pos end
	end
	local p = L.owner_pos(ctx) or { 0, 0, 0 }
	for tm = 1, 2 do
		if out[tm] == nil then out[tm] = L.snap({ p[1] + 15 * (tm == 1 and 1 or -1), p[2], p[3] + 15 }) end
	end
	return out
end

local function bench_pos(b, team, i)
	local base = b[team]
	return L.snap({ base[1] + 2 * i, base[2], base[3] + 2 })
end

-- P0.13 round ---------------------------------------------------------------------------------------------------
local function step_round(ctx)
	local b = M.benches(ctx)
	local list = {}
	for i, team in ipairs({ 1, 2, 2 }) do
		local bot = L.spawn(ctx, team, team == 1 and 'M16A3' or 'AK74M', bench_pos(b, team, i), { 0, 0, 1 }, 0)
		if bot then list[#list + 1] = bot end
	end
	local function started()
		local fl = S.flags()
		if #fl == 0 then return false end
		for _, cp in ipairs(fl) do
			if cp.isInPreRound == true then return false end
		end
		return true
	end
	local ok = ctx:wait_until(started, 60, 'preround')
	local val = { bots = #list, flags = #S.flags(), started = ok }
	if not ok then
		result(ctx, 'P0.13', false, val, 'round: preround (expected if bots do not count; RUNBOOK: -DisablePreRound)')
		runner().p0.round = 'preround'
		return
	end
	-- ticket cost of a spawn, a Kill and a DeletePlayer (team 2), 5 s apart [live:L16]
	local function t2() return S.tickets(2) or 0 end
	local probe = ctx:bot(2, 'AK74M')
	local tc = { spawn = 1, kill = 1, remove = 1 }
	if probe then
		local a = t2()
		L.respawn(ctx, probe, bench_pos(b, 2, 5), { 0, 0, 1 }, 0)
		ctx:wait(5)
		local b1 = t2()
		bots.kill(probe)
		ctx:wait(5)
		local b2 = t2()
		L.respawn(ctx, probe, bench_pos(b, 2, 5), { 0, 0, 1 }, 0)
		ctx:wait(5)
		local b3 = t2()
		ctx:drop_bot(probe)
		ctx:wait(5)
		local b4 = t2()
		tc = { spawn = a - b1, kill = b1 - b2, remove = b3 - b4 }
		runner().tcost = { spawn = math.max(0, tc.spawn), kill = math.max(0, tc.kill), remove = math.max(0, tc.remove) }
	end
	runner().p0.round = 'started'
	val.ts, val.tk, val.tr = tc.spawn, tc.kill, tc.remove
	result(ctx, 'P0.13', true, val, string.format('round: started, tickets per spawn %g, kill %g, remove %g', tc.spawn,
		tc.kill, tc.remove))
	-- the 3 bots stay benched for P0.15 (removed at cleanup unless P0.15 runs next in the same call)
	ctx.keep = list
end

-- P0.15 level: round restart keeps the Lua state and the bot players? [live:L22] ----------------------------------
local function step_level(ctx, args)
	if args.confirm ~= 'confirm' then
		ctx:say('p0 level restarts the round: type !mh p0 level confirm')
		return
	end
	local b = M.benches(ctx)
	local list = {}
	for i, team in ipairs({ 1, 2 }) do
		local bot = L.spawn(ctx, team, team == 1 and 'M16A3' or 'AK74M', bench_pos(b, team, i), { 0, 0, 1 }, 0)
		if bot then list[#list + 1] = bot end
	end
	ids.counter = 4242
	local RUN = runner()
	local epoch = RUN.level_epoch
	ctx.expect_level = true
	if not RUN.send_admin('rr', ids.player(ctx.owner)) then
		result(ctx, 'P0.15', false, { sent = false }, 'level: restartRound refused by RCON')
		return
	end
	local okl = ctx:wait_until(function() return RUN.level_epoch > epoch and C.gk - RUN.level_k > 15 * C.hzv end, 240,
		'level')
	local players_ok = 0
	for _, bot in ipairs(list) do
		local ok, same = pcall(function() return PlayerManager:GetPlayerById(bot.pl.id) ~= nil end)
		if ok and same then players_ok = players_ok + 1 end
	end
	local kept = okl and ids.counter == 4242 and players_ok == #list
	local b2 = M.benches(ctx)
	for i, bot in ipairs(list) do L.respawn(ctx, bot, bench_pos(b2, bot.tm, i), { 0, 0, 1 }, 0) end
	RUN.p0.level = kept and 'kept' or 'lost'
	result(ctx, 'P0.15', kept, { reload = okl, counter = ids.counter == 4242, players = players_ok, of = #list },
		kept and 'level: state kept (rr and nextmap allowed in the queue)' or
		'level: state lost (no rr/nextmap tonight; see the RUNBOOK)')
end

-- P0.3 tick ------------------------------------------------------------------------------------------------------
local function step_tick(ctx)
	local ms0 = SharedUtils:GetTimeMS()
	local k0 = C.gk
	local ok = ctx:wait_until(function() return SharedUtils:GetTimeMS() - ms0 >= 10000 end, 30, 'tick')
	local n = C.gk - k0
	local ms = SharedUtils:GetTimeMS() - ms0
	local hz = SharedUtils:GetTickrate()
	local f = meta.freq()
	local expect = 10 * C.hz()
	local pass = ok and math.abs(n - expect) <= math.max(3, expect // 100)
	result(ctx, 'P0.3', pass, { n = n, ms = ms, hz = hz, freq = f, clk = C.mode },
		string.format('tick: %.1f Hz, %d ticks / %.1f s, mode %s', hz, n, ms / 1000, f))
end

-- P0.5 aim calibration from a bot alone (DESIGN 3.9) [live:L08] --------------------------------------------------------
local function shot_dir_since(k0, sid)
	for i = #H.recent_shots, 1, -1 do
		local s = H.recent_shots[i]
		if s.k >= k0 and s.sid == sid then
			for j = #H.recent_hits, 1, -1 do
				local h = H.recent_hits[j]
				if h.uid == s.uid and h.uid ~= 0 then return V.norm(V.sub(h.pos, s.pos)), 'hit' end
			end
			return V.norm(s.dir), 'shot'
		end
	end
	for j = #H.recent_hits, 1, -1 do
		local h = H.recent_hits[j]
		if h.k >= k0 and h.shooter == sid then return nil, 'hit-only', h.pos end
	end
	return nil, 'none'
end

local function calibrate(ctx, bot, settings)
	local obs = {}
	for _, y in ipairs(settings) do
		bots.aim(bot, y, settings.pitch)
		ctx:wait(0.6)
		local so = bots.soldier(bot)
		local body = so and V.horiz(V.v(so.worldTransform.forward)) or nil
		local k0 = C.gk
		L.fire1(ctx, bot)
		ctx:wait(0.4)
		local d, src, hp = shot_dir_since(k0, bot.sid)
		if d == nil and hp then d = V.norm(V.sub(hp, bots.eye(bot))) end
		obs[#obs + 1] = { y = y, p = settings.pitch, body = body, shot = d, src = src }
	end
	local best, berr = nil, math.huge
	for _, cand in ipairs(V.candidates()) do
		local e = 0
		for _, o in ipairs(obs) do
			local pred = V.aim_to_dir(cand, o.y, o.p)
			local ref = o.shot or o.body
			if ref == nil then
				e = e + 180
			elseif o.shot then
				e = e + V.angle_deg(pred, ref)
			else
				e = e + V.angle_deg(V.horiz(pred), ref)
			end
		end
		e = e / #obs
		if e < berr then best, berr = cand, e end
	end
	local spread = 0
	for i = 2, #obs do
		local a, b2 = obs[1].shot or obs[1].body, obs[i].shot or obs[i].body
		if a and b2 then spread = math.max(spread, V.angle_deg(a, b2)) end
	end
	return best, berr, spread, obs
end

local function step_aim(ctx)
	local p, f = front(ctx)
	if p == nil then
		result(ctx, 'P0.5', false, { why = 'no user soldier' }, 'aim: spawn first')
		return
	end
	local pos = L.snap(V.add(p, V.scale(V.yrot(f, math.pi / 2), 4)))
	local bot = L.spawn(ctx, 1, 'M16A3', pos, f, 0)
	if bot == nil then
		result(ctx, 'P0.5', false, { why = 'spawn failed' }, 'aim: bot spawn failed (L07)')
		return
	end
	bots.zoom(bot, true)
	ctx:wait(1.0)
	local settings = { 0.0, 0.5, 1.0, 1.5, pitch = -0.1 }
	local cal, err, spread = calibrate(ctx, bot, settings)
	if spread < 5 then
		settings = { 0.0, 30.0, 60.0, 90.0, pitch = -6.0 }
		cal, err, spread = calibrate(ctx, bot, settings)
	end
	if spread < 5 and bots.pass == 'presim' then
		bots.pass = 'postsim'
		ctx:mark('aim-pass-postsim')
		cal, err, spread = calibrate(ctx, bot, { 0.0, 0.5, 1.0, 1.5, pitch = -0.1 })
	end
	local val = { err = err, spread = spread, pass = bots.pass }
	if cal and spread >= 5 then
		bots.cal = cal
		bots.cal_src = 'bot'
		val.sx, val.sz, val.off, val.unit, val.ps = cal.sx, cal.sz, cal.off, cal.unit, cal.ps
	end
	-- verification: 5 ADS shots at the wall (if recorded): centre of impacts vs the aim point
	local wall = sites.get('wall')
	local verr = nil
	if bots.cal and wall then
		local sp = L.snap(V.add(wall.pos, V.scale(wall.n, 10)))
		bots.place(bot, sp, V.scale(wall.n, -1))
		bots.zoom(bot, true)
		ctx:wait(1.0)
		bots.aim_at(bot, wall.pos)
		ctx:wait(0.5)
		local k0 = C.gk
		for _ = 1, 5 do
			L.fire1(ctx, bot)
			ctx:wait(1.0)
		end
		local e = bots.eye(bot)
		local acc, n = { 0, 0, 0 }, 0
		for _, h in ipairs(H.recent_hits) do
			if h.k >= k0 and h.shooter == bot.sid then
				acc = V.add(acc, h.pos)
				n = n + 1
			end
		end
		if n > 0 and e then verr = V.angle_deg(V.sub(V.scale(acc, 1 / n), e), V.sub(wall.pos, e)) end
		val.verr = verr
		val.vn = n
	end
	-- cross-check, logged only: the user's camera and AimingSimulation (client)
	local sid = ids.player(ctx.owner)
	local cam = sites.cams[sid]
	if cam and bots.cal then
		local yw, pt = V.dir_to_aim(bots.cal, cam.fwd)
		val.cam_yaw, val.cam_pitch, val.as_yaw, val.as_pitch = yw, pt, cam.ay, cam.ap
	end
	local ok = bots.cal ~= nil and err < 0.5 and (verr == nil or verr < 0.5)
	result(ctx, 'P0.5', ok, val, bots.cal and string.format('aim: convention sx%d sz%d off%d %s ps%d, error %.2f deg%s',
		bots.cal.sx, bots.cal.sz, math.floor(math.deg(bots.cal.off) + 0.5), bots.cal.unit, bots.cal.ps, err,
		verr and string.format(', wall check %.2f deg', verr) or ', no wall check (record !mh wall first)') or
		'aim: the bot did not turn (L08)')
	meta.session_meta(runner())
end

-- P0.6 bots -------------------------------------------------------------------------------------------------------
local function step_bots(ctx)
	local p, f = front(ctx)
	if p == nil then
		result(ctx, 'P0.6', false, { why = 'no user soldier' }, 'bots: spawn first')
		return
	end
	local passed, total = 0, 0
	local val = {}
	local function check(name, ok)
		total = total + 1
		if ok then passed = passed + 1 end
		val[name] = ok == true
	end
	local b1 = L.spawn(ctx, 1, 'M16A3', L.snap(V.add(p, V.scale(f, 5))), V.scale(f, -1), 0, { secondary = 'M9' })
	local b2 = L.spawn(ctx, 2, 'AK74M', L.snap(V.add(p, V.scale(f, 10))), V.scale(f, -1), 0)
	check('spawn', b1 ~= nil and b2 ~= nil)
	if b1 == nil then
		result(ctx, 'P0.6', false, val, 'bots: spawn failed (L07)')
		return
	end
	S.watch(b1.sid, { hp = true, wf = true })
	local wname = bots.weapon(b1)
	check('weapon', wname ~= nil)
	local poses_ok = true
	for _, pose in ipairs({ 0, 1, 2, 0 }) do
		bots.pose(b1, pose)
		ctx:wait(2.0)
		if bots.pose_of(b1) ~= pose then poses_ok = false end
	end
	check('pose', poses_ok)
	local p0 = bots.pos(b1)
	local target = V.add(p0, V.scale(V.yrot(f, math.pi / 2), 2))
	bots.place(b1, target)
	ctx:wait(0.5)
	local p1 = bots.pos(b1)
	check('place', p1 ~= nil and math.abs(V.dist(p0, p1) - 2) < 0.3)
	-- throttle for 1 s; velocity from the soldier (L10), else from positions
	local vmax = 0
	local q0 = bots.pos(b1)
	bots.face(b1, V.scale(f, -1))
	bots.hold(b1, EntryInputActionEnum.EIAThrottle, 1.0)
	for _ = 1, C.hzv do
		ctx:wait_ticks(1)
		local so = bots.soldier(b1)
		if so then
			local okv, v = pcall(function() return V.len(V.v(so.velocity)) end)
			if okv and v > vmax then vmax = v end
		end
	end
	bots.release(b1, EntryInputActionEnum.EIAThrottle)
	local moved = q0 and bots.pos(b1) and V.dist(q0, bots.pos(b1)) or 0
	val.vmax = vmax
	val.moved = moved
	check('move', vmax > 1 or moved > 1)
	-- slot switch to the M9 (slot 1) [live:L31]
	local s0 = bots.slot_of(b1)
	bots.select_slot(b1, 1)
	local sw = ctx:wait_until(function() return bots.slot_of(b1) == 1 end, 2, 'slot')
	val.slot0 = s0
	check('slot', s0 == 0 and sw)
	bots.select_slot(b1, 0)
	ctx:wait(1.0)
	-- cross-faction SelectWeapon probe (logged only, L29)
	if b2 then
		local okx = pcall(function() bots.equip(b2, 'M16A3') end)
		val.xfaction = okx
	end
	local n0 = PlayerManager:GetPlayerCount()
	S.unwatch(b1.sid)
	ctx:drop_bot(b1)
	if b2 then ctx:drop_bot(b2) end
	ctx:wait(1.0)
	local n1 = PlayerManager:GetPlayerCount()
	check('remove', n1 == n0 - (b2 and 2 or 1))
	result(ctx, 'P0.6', passed == total, val, string.format('bots %s %d/%d%s', passed == total and 'ok' or 'FAIL', passed,
		total, passed == total and '' or (' (failed: ' .. M.failed(val) .. ')')))
end

function M.failed(val)
	local out = {}
	for key, v in pairs(val) do if v == false then out[#out + 1] = key end end
	table.sort(out)
	return table.concat(out, ',')
end

-- P0.7 fire + P0.8 geometry + P0.11 cadence/units ------------------------------------------------------------------------
local function median(t)
	if #t == 0 then return nil end
	table.sort(t)
	local m = (#t + 1) // 2
	if #t % 2 == 1 then return t[m] end
	return (t[m] + t[m + 1]) / 2
end

local function step_fire(ctx)
	local wall = ctx:need_site('wall')
	if wall == nil then
		result(ctx, 'P0.7', false, { why = 'no wall' }, 'fire: record !mh wall first')
		return
	end
	if bots.cal == nil then
		result(ctx, 'P0.7', false, { why = 'no aimcal' }, 'fire: run !mh p0 aim first')
		return
	end
	local sp = L.snap(V.add(wall.pos, V.scale(wall.n, 10)))
	local bot = L.spawn(ctx, 1, 'M16A3', sp, V.scale(wall.n, -1), 0)
	if bot == nil then
		result(ctx, 'P0.7', false, { why = 'spawn' }, 'fire: spawn failed')
		return
	end
	S.watch(bot.sid, { hp = true, gs = true, wf = true })
	ctx:wait(1.0)
	bots.aim_at(bot, wall.pos)
	L.wait_clear(ctx, bots.eye(bot), wall.pos)
	local fl0 = bots.fire_logic(bot)
	local srv0, poll0 = S.gs_srv_seen, S.gs_poll_seen
	local agree0n, agree0ok = S.agree.n, S.agree.ok
	local fs0 = S.counter(bot.sid).fs
	local k0 = C.gk
	for _ = 1, 5 do
		L.fire1(ctx, bot)
		ctx:wait(1.5)
	end
	local auto = L.ensure_fire(ctx, bot, 2)
	local fl1 = bots.fire_logic(bot)
	local okn, nb = L.fire_n(ctx, bot, 5, 800)
	ctx:wait(1.0)
	-- collect
	local shots, hits = {}, {}
	for _, s in ipairs(H.recent_shots) do if s.k >= k0 and s.sid == bot.sid then shots[#shots + 1] = s end end
	for _, h in ipairs(H.recent_hits) do if h.k >= k0 and h.shooter == bot.sid then hits[#hits + 1] = h end end
	local on_plane = 0
	for _, h in ipairs(hits) do
		if math.abs(V.dot(V.sub(h.pos, wall.pos), wall.n)) < 0.05 then on_plane = on_plane + 1 end
	end
	local nup_max = 0
	for _, v in pairs(S.nup) do if v > nup_max then nup_max = v end end
	local srv = S.gs_srv_seen - srv0
	local poll = S.gs_poll_seen - poll0
	local fsn = S.counter(bot.sid).fs - fs0
	local agree = (S.agree.n - agree0n) > 0 and (S.agree.ok - agree0ok) == (S.agree.n - agree0n)
	runner().p0.gs = srv > 0 and 'srv' or (poll > 0 and 'poll' or 'none')
	runner().p0.auto = auto
	local val = { shots = #shots, hits = #hits, plane = on_plane, fl0 = fl0, fl = fl1, auto = auto, burst = nb,
		fs = fsn, nup = nup_max, srv = srv, poll = poll, agree = agree, gsrc = runner().p0.gs }
	for tn, v in pairs(H.ishit) do val['is_' .. tn] = v end
	local ok = #shots == 10 and on_plane >= 9 and fl1 == 2 and fsn == 10 and nup_max <= 1 and srv > 0 and agree
	result(ctx, 'P0.7', ok, val, string.format('fire: shots %d, hits %d, auto %s, gs %s %d/tick%s', #shots, on_plane,
		auto and 'yes' or 'NO (W-A runs a and c only)', runner().p0.gs, nup_max, okn and '' or ', burst timeout'))
	-- P0.8 geometry: |hit - ray/plane| and angle(shot dir, origin->hit)
	local dpos, dang = {}, {}
	for _, s in ipairs(shots) do
		for _, h in ipairs(hits) do
			if h.uid == s.uid and h.uid ~= 0 then
				local x = V.ray_plane(s.pos, s.dir, wall.pos, wall.n)
				if x then dpos[#dpos + 1] = V.dist(x, h.pos) end
				dang[#dang + 1] = V.angle_deg(s.dir, V.sub(h.pos, s.pos))
			end
		end
	end
	local mp, ma = median(dpos), median(dang)
	-- 0.05 deg: bullet drop over the first tick's chord (~0.02 deg at 10 m) passes, a pre-dispersion direction
	-- (>= 0.2 deg off for ADS, 2.5 deg hip) does not
	local geo = mp ~= nil and mp <= 0.01 and ma <= 0.05
	if mp ~= nil and ma ~= nil and ma > 90 then
		H.dir_sign = -H.dir_sign
		ctx:mark('shot-dir-flipped')
	end
	result(ctx, 'P0.8', geo, { dpos = mp, dang = ma, n = #dpos },
		mp and string.format('geometry: hit vs ray %.1f cm, angle %.3f deg -> %s', mp * 100, ma,
			geo and 'spawn dir is post-dispersion, hit pos exact' or 'use origin->hit only') or
		'geometry: no linked shot/hit pairs (W1 uses impacts only)')
	-- P0.11 units: da at rest vs the data minimum (M16A3 stand hip 2.5 deg), both readings
	local g = S.last_gs[bot.sid]
	local units, angle = 'unknown', 'unknown'
	if g and g.dmin and g.dmin > 0 then
		local d = g.dmin
		local cands = { { 2.5, 'deg', 'half' }, { 5.0, 'deg', 'full' }, { math.rad(2.5), 'rad', 'half' },
			{ math.rad(5.0), 'rad', 'full' } }
		for _, c in ipairs(cands) do
			if math.abs(d - c[1]) / c[1] < 0.01 then units, angle = c[2], c[3] end
		end
	end
	runner().p0.units = units
	runner().p0.angle = angle
	result(ctx, 'P0.11', units ~= 'unknown', { dmin = g and g.dmin, da = g and g.da, units = units, angle = angle,
		ra = g and g.ra, rr = g and g.rr, seed = g and g.seed },
		string.format('units: dmin %s -> %s %s', g and string.format('%.4g', g.dmin or -1) or '?', units, angle))
end

-- P0.9 damage: YES / NO (arbitration) / NO (aim) [live:L14] -------------------------------------------------------------
local function step_damage(ctx)
	if bots.cal == nil then
		result(ctx, 'P0.9', false, { why = 'no aimcal' }, 'damage: run !mh p0 aim first')
		return
	end
	local p, f = front(ctx)
	if p == nil then
		result(ctx, 'P0.9', false, { why = 'no user soldier' }, 'damage: spawn first')
		return
	end
	local side = V.yrot(f, math.pi / 2)
	local spos = L.snap(V.add(p, V.scale(side, 3)))
	local tpos = L.snap(V.add(spos, V.scale(f, 10)))
	local sh = L.spawn(ctx, 1, 'M16A3', spos, f, 0)
	local tg = L.spawn(ctx, 2, 'AK74M', tpos, V.scale(f, -1), 0)
	if sh == nil or tg == nil then
		result(ctx, 'P0.9', false, { why = 'spawn' }, 'damage: spawn failed')
		return
	end
	S.watch(tg.sid, { hp = true })
	bots.zoom(sh, true)
	ctx:wait(1.0)
	local k0 = C.gk
	for _ = 1, 5 do
		bots.aim_at(sh, L.chest(tg))
		ctx:wait(0.2)
		L.fire1(ctx, sh)
		ctx:wait(0.6)
	end
	ctx:wait(0.5)
	local n_dmg = 0
	for _, d in ipairs(H.dmg_log) do
		if d.k >= k0 and d.vic == tg.sid and d.giver == sh.sid and d.hp1 < d.hp0 then n_dmg = n_dmg + 1 end
	end
	local n_hit = L.hits_since(k0, tg.sid)
	local verdict = n_dmg >= 3 and 'YES' or (n_hit > 0 and 'NO-arb' or 'NO-aim')
	runner().p0.damage = verdict
	local text = verdict == 'YES' and 'bot damage: YES' or (verdict == 'NO-arb' and 'bot damage: NO (arbitration)' or
		'bot damage: NO (aim) - run !mh p0 aim, then !mh p0 damage again')
	result(ctx, 'P0.9', verdict == 'YES', { dmg = n_dmg, hits = n_hit, verdict = verdict }, text)
end

-- P0.10 human shots seen server-side? [live:L25] ---------------------------------------------------------------------
local function step_human(ctx)
	local sid = ids.player(ctx.owner)
	H.human_watch[sid] = true
	S.watch(sid, { wf = true, gs = true })
	local k0 = C.gk
	ctx:prompt('fire3', { n = 1, of = 1 }, 'fire 3 rounds at a wall now (30 s)')
	local function seen()
		local n = 0
		for _, s in ipairs(H.recent_shots) do if s.k >= k0 and s.sid == sid then n = n + 1 end end
		local m = 0
		for _, h in ipairs(H.recent_hits) do if h.k >= k0 and h.shooter == sid then m = m + 1 end end
		return n >= 3 or m >= 3, n, m
	end
	ctx:wait_until(function() return (seen()) end, 30, 'human')
	local _, n, m = seen()
	H.human_watch[sid] = nil
	S.unwatch(sid)
	runner().p0.human = n > 0
	result(ctx, 'P0.10', n >= 3, { shots = n, hits = m }, string.format('human shots seen: %s (shots %d, hits %d)',
		n > 0 and 'yes' or 'no', n, m))
end

-- P0.12 log: a ~950 B test row (the extractor must read it intact) ----------------------------------------------------
local function step_log(ctx)
	local val = {}
	for i = 1, 44 do val[string.format('a%02d', i)] = 1.23456789 + i end
	R.row('p0', { step = 'P0.12', ok = true, val = val })
	ctx:say('log: test row written; run extract.py --dry-run (RUNBOOK step 9)')
end

-- P0.14 report ----------------------------------------------------------------------------------------------------
local ORDER = { 'P0.13', 'P0.15', 'P0.3', 'P0.5', 'P0.6', 'P0.7', 'P0.8', 'P0.11', 'P0.9', 'P0.10' }

local function step_report(ctx)
	local rp = runner().p0
	local steps = rp.steps or {}
	local parts = {}
	for _, s in ipairs(ORDER) do
		local r = steps[s]
		parts[#parts + 1] = s .. ' ' .. (r == nil and '-' or (r.ok and 'ok' or 'FAIL'))
	end
	ctx:say(table.concat(parts, ', '))
	local rr = rp.level == 'kept'
	local q
	if not rr then
		q = '!mh queue WA:tier1 SB MB WA:rest'
	elseif rp.damage == 'YES' then
		q = '!mh queue rr WA:tier1 WB:B SA:B SB MB WA:rest rr MA nextmap MA'
	else
		q = '!mh queue rr WA:tier1 SB MB WA:rest rr MA nextmap MA'
	end
	ctx:say('bot damage ' .. tostring(rp.damage or '?') .. ', gs ' .. tostring(rp.gs or '?') .. ', auto ' ..
		tostring(rp.auto) .. ', rr ' .. (rr and 'yes' or 'no'))
	ctx:say('overnight: ' .. q)
	R.row('p0', { step = 'P0.14', ok = true, val = { damage = rp.damage or 'unknown', gs = rp.gs or 'unknown',
		rr = rr, units = rp.units or 'unknown', angle = rp.angle or 'unknown', auto = rp.auto == true } })
end

local STEPS = { round = step_round, level = step_level, tick = step_tick, aim = step_aim, bots = step_bots,
	fire = step_fire, damage = step_damage, human = step_human, log = step_log, report = step_report }
M.STEPS = STEPS

function M.needs(args)
	return {}
end

function M.cost(args)
	local s = args.step or 'all'
	if s == 'round' then return { s1 = 1, s2 = 4, d2 = 1, r2 = 1 } end
	if s == 'level' then return { s1 = 2, s2 = 2 } end
	if s == 'all' then return { s1 = 5, s2 = 3, r1 = 5, r2 = 3 } end
	return { s1 = 2, s2 = 1, r1 = 2, r2 = 1 }
end

function M.estimate(args)
	local t = { round = 90, level = 60, tick = 12, aim = 40, bots = 25, fire = 25, damage = 15, human = 30, log = 1,
		report = 1, all = 130 }
	return t[args.step or 'all'] or 30
end

function M.run(ctx, args)
	local step = args.step or 'report'
	if step == 'all' then
		for _, s in ipairs({ 'tick', 'aim', 'bots', 'fire', 'damage' }) do
			STEPS[s](ctx, args)
			for _, b in ipairs(ctx.bots) do pcall(function() S.unwatch(b.sid) end) end
			while #ctx.bots > 0 do ctx:drop_bot(ctx.bots[1]) end
		end
		return
	end
	local fn = STEPS[step]
	if fn == nil then
		ctx:say('p0 steps: round level tick aim bots fire damage human log report all')
		return
	end
	fn(ctx, args)
end

function M.cleanup(ctx) end

return M

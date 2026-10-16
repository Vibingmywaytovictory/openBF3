-- Scenario helpers that wait (DESIGN.md sections 3.8, 3.9, 6.2). Every wait goes through ctx (coroutine body only,
-- never inside an engine callback: apicheck rule 12). Fixtures are delegated to mh_bots.
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local sites = require('mh_sites')
local ids = require('mh_ids')
local W = require('scenarios/wa_cfg')

local L = {}

L.POSES = { stand = 0, crouch = 1, prone = 2 }
L.FIRE = EntryInputActionEnum.EIAFire
L.CHEST = 1.35   -- chest height above the feet [inferred]
L.HEAD = 1.65

function L.scale(args, n)
	local s = tonumber(args.scale or '1') or 1
	if s >= 1 then return n end
	return math.max(1, math.floor(n * s + 0.5))
end

function L.wait_alive(ctx, bot, timeout)
	return ctx:wait_until(function() return bots.alive(bot) end, timeout or 5, 'alive')
end

-- spawn(ctx, team, w, pos, dir, pose, opt) -> bot | nil: create (once) + spawn + weapon and slot checks.
function L.spawn(ctx, team, w, pos, dir, pose, opt)
	local bot = ctx:bot(team, w, opt)
	if bot == nil then return nil end
	if not L.respawn(ctx, bot, pos, dir, pose) then return nil end
	return bot
end

function L.respawn(ctx, bot, pos, dir, pose)
	if bots.alive(bot) then
		if pos then bots.place(bot, pos, dir) end
		if pose then bots.pose(bot, pose) end
		return true
	end
	if not bots.spawn(bot, pos, dir, pose or 0) then return false end
	if not L.wait_alive(ctx, bot, 5) then return false end
	ctx:wait(0.3)
	if dir then bots.face(bot, dir) end
	L.ensure_slot(ctx, bot)
	return true
end

function L.ensure_slot(ctx, bot)
	if bots.slot_of(bot) == bot.wslot then return true end
	bots.select_slot(bot, bot.wslot)
	return ctx:wait_until(function() return bots.slot_of(bot) == bot.wslot end, 2, 'slot')
end

-- ensure_fire(ctx, bot, want): cycle with EIACycleFireMode until fireLogic == want (<= 4 presses) [live:L30].
function L.ensure_fire(ctx, bot, want)
	for _ = 1, 5 do
		local fl = bots.fire_logic(bot)
		if fl == want then return true end
		if fl < 0 then return false end
		bots.cycle_fire(bot)
		ctx:wait(0.3)
	end
	return bots.fire_logic(bot) == want
end

function L.shots(bot)
	return S.shots(bot.sid)
end

-- fire1: hold EIAFire until one attributed shot (or 6 ticks), then release.
function L.fire1(ctx, bot)
	local n0 = L.shots(bot)
	bots.hold(bot, L.FIRE, 1.0)
	local ok = ctx:wait_until(function() return L.shots(bot) > n0 end, 0.25, 'fire1')
	bots.release(bot, L.FIRE)
	return ok
end

-- fire_n: hold until n shots or the timeout n*60/rpm + 1 s.
function L.fire_n(ctx, bot, n, rpm)
	local n0 = L.shots(bot)
	bots.hold(bot, L.FIRE, 1.0)
	local ok = ctx:wait_until(function() return L.shots(bot) >= n0 + n end, n * 60 / (rpm or 600) + 1.0, 'fire')
	bots.release(bot, L.FIRE)
	return ok, L.shots(bot) - n0
end

function L.ammo(bot)
	local w = bots.weapon(bot)
	if w == nil then return 0, 0 end
	return math.tointeger(w.primaryAmmo) or 0, math.tointeger(w.secondaryAmmo) or 0
end

function L.reload(ctx, bot, mag)
	local a = L.ammo(bot)
	if a >= mag then return true end
	bots.press(bot, EntryInputActionEnum.EIAReload, 2)
	return ctx:wait_until(function() return (L.ammo(bot)) >= mag end, 6, 'reload')
end

-- ensure_ammo(ctx, bot, need, mag): reload if the magazine cannot hold the next unit; respawn if the reserve is empty.
function L.ensure_ammo(ctx, bot, need, mag, pos, dir, pose)
	local a, r = L.ammo(bot)
	if a >= need then return true end
	if r <= 0 then
		bots.kill(bot)
		ctx:wait(0.5)
		if not L.respawn(ctx, bot, pos, dir, pose) then return false end
		return true
	end
	return L.reload(ctx, bot, math.min(mag, a + r))
end

function L.owner_pos(ctx)
	local pl = ctx.owner
	if pl == nil then return nil end
	local ok, p = pcall(function() return V.v(pl.soldier.worldTransform.trans) end)
	if ok then return p end
	return nil
end

-- seg_dist(p, a, b): distance of point p from the segment a-b.
function L.seg_dist(p, a, b)
	local ab = V.sub(b, a)
	local l2 = V.dot(ab, ab)
	if l2 < 1e-9 then return V.dist(p, a) end
	local s = math.max(0, math.min(1, V.dot(V.sub(p, a), ab) / l2))
	return V.dist(p, V.add(a, V.scale(ab, s)))
end

-- wait_clear(ctx, a, b): pause while any human stands within 1 m of the line of fire a-b.
function L.wait_clear(ctx, a, b)
	local function clear()
		for _, pl in ipairs(ids.human_list()) do
			local ok, p = pcall(function() return V.v(pl.soldier.worldTransform.trans) end)
			if ok and p then
				local mid = { p[1], p[2] + 1.0, p[3] }
				if L.seg_dist(mid, a, b) < 1.0 then return false end
			end
		end
		return true
	end
	if clear() then return true end
	ctx:say('step out of the line of fire')
	return ctx:wait_until(clear, 120, 'clear')
end

-- Ground-snapped point; falls back to the given height.
function L.snap(p)
	local g = sites.ground(p, 3.0, 8.0)
	if g then return g end
	return p
end

function L.chest(bot, zone)
	local p = bots.pos(bot)
	if p == nil then return nil end
	return { p[1], p[2] + (zone == 'head' and L.HEAD or L.CHEST), p[3] }
end

function L.wait_for(ctx, s)
	ctx:wait(s)
end

function L.dmg_pairs_since(k0, vic, giver)
	local n, last = 0, nil
	for _, d in ipairs(H.dmg_log) do
		if d.k >= k0 and d.vic == vic and (giver == nil or d.giver == giver) then
			n = n + 1
			last = d
		end
	end
	return n, last
end

function L.hits_since(k0, vic)
	local n = 0
	for _, h in ipairs(H.recent_hits) do
		if h.k >= k0 and h.vic == vic then n = n + 1 end
	end
	return n
end

function L.team_for(w)
	return W.teams[w] or 1
end

function L.kgk()
	return C.gk
end

return L

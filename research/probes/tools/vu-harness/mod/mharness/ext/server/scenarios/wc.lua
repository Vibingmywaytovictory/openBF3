-- W-C explosives (DESIGN.md 6.9). U (the user fires). Item W7.
-- RPG-7 and SMAW direct hits at a target 30 m along the range (or the user's view), M320 HE direct and offset, M67
-- ring of 8 targets at radii 0.5..7 m around the ring site. Detonation point: the last per-tick position of the tracked
-- projectile (shot kind missile/grenade) or an ExplosionEntityData creation (ev explosion) [live:L12].
-- Args: parts=rpg,smaw,m320,m67, scale (tests).
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local ids = require('mh_ids')
local sites = require('mh_sites')
local L = require('scenarios/lib')

local M = { id = 'WC', title = 'W-C explosives' }

M.RADII = { 0.5, 1, 2, 3, 4, 5, 6, 7 }

function M.needs(args) return {} end
function M.cost(args) return { s2 = 40, d2 = 35, r2 = 9 } end
function M.estimate(args) return 900 end

local function want(args, p)
	return (args.parts or 'rpg,smaw,m320,m67'):find(p, 1, true) ~= nil
end

local function target_spot(ctx)
	local rng = sites.get('range')
	local p = L.owner_pos(ctx)
	if rng then return L.snap(V.add(rng.pos, V.scale(rng.dir, 30))), V.scale(rng.dir, -1) end
	if p == nil then return nil end
	local sid = ids.player(ctx.owner)
	local cam = sites.cams[sid]
	local f = cam and V.horiz(cam.fwd) or { 0, 0, 1 }
	return L.snap(V.add(p, V.scale(f, 30))), V.scale(f, -1)
end

local function direct(ctx, T, tpos, face, user, label, w, n)
	for r = 1, n do
		if not bots.alive(T) then
			ctx:wait(1.0)
			if not L.respawn(ctx, T, tpos, face, 0) then return false end
		end
		bots.place(T, tpos, face)
		ctx:wait_until(function() return bots.health(T) >= 100 end, 25, 'full')
		local k0 = C.gk
		ctx:prompt('fire', { n = r, of = n, tid = T.sid, zone = label },
			string.format('%s: hit the target directly (%d/%d)', w, r, n))
		ctx:mark('shot-req', { cfg = label, i = r, k0 = C.kk() })
		ctx:wait_until(function() return (L.dmg_pairs_since(k0, T.sid, user)) > 0 end, 120, 'hit')
		ctx:wait(2.0)
	end
	return true
end

function M.run(ctx, args)
	local user = ids.player(ctx.owner)
	H.human_watch[user] = true
	S.watch(user, { hp = true })
	local tpos, face = target_spot(ctx)
	if tpos == nil then ctx:abort('no user soldier') end
	local T = L.spawn(ctx, 2, 'AK74M', tpos, face, 0)
	if T == nil then ctx:abort('target spawn failed') end
	S.watch(T.sid, { hp = true })
	local n = L.scale(args, 3)
	local cfgs = {}
	if want(args, 'rpg') then cfgs[#cfgs + 1] = { 'rpg', 'RPG-7 (RU engineer)' } end
	if want(args, 'smaw') then cfgs[#cfgs + 1] = { 'smaw', 'SMAW (US engineer)' } end
	if want(args, 'm320') then cfgs[#cfgs + 1] = { 'm320', 'M320 HE (assault)' } end
	if want(args, 'm67') then cfgs[#cfgs + 1] = { 'm67', 'M67 grenade' } end
	for i, c in ipairs(cfgs) do
		local key, w = c[1], c[2]
		ctx:config(i, #cfgs, key, nil, function()
			ctx:prompt('equip', { n = 1, of = 1 }, 'equip the ' .. w .. ', then !mh ok')
			ctx.ok_flag = false
			if not ctx:wait_until(function() return ctx.ok_flag end, 300, 'equip') then return false end
			if key ~= 'm67' then
				if not direct(ctx, T, tpos, face, user, key, w, n) then return false end
				if key == 'm320' then
					for r = 1, n do
						local k0 = C.gk
						ctx:prompt('fire', { n = r, of = n, tid = T.sid, zone = 'offset' },
							string.format('M320: land a round 1-3 m beside the target (%d/%d)', r, n))
						ctx:mark('shot-req', { cfg = 'm320-offset', i = r, k0 = C.kk() })
						ctx:wait_until(function() return (L.dmg_pairs_since(k0, T.sid, user)) > 0 end, 120, 'hit')
						ctx:wait(3.0)
						if not bots.alive(T) then L.respawn(ctx, T, tpos, face, 0) end
					end
				end
				return true
			end
			local ring = sites.get('ring')
			if ring == nil then
				ctx:say('M67: look at an open spot on the ground and type !mh ring, then !mh ok')
				ctx.ok_flag = false
				ctx:wait_until(function() return ctx.ok_flag and sites.get('ring') ~= nil end, 300, 'ring')
				ring = sites.get('ring')
				if ring == nil then return false end
			end
			bots.bench(T, L.snap(V.add(tpos, { 0, 0, 8 })), face)
			local ringbots = {}
			for j, rad in ipairs(M.RADII) do
				local a = 2 * math.pi * (j - 1) / #M.RADII
				local p = L.snap({ ring.pos[1] + rad * math.cos(a), ring.pos[2], ring.pos[3] + rad * math.sin(a) })
				local b = L.spawn(ctx, 2, 'AK74M', p, { -math.cos(a), 0, -math.sin(a) }, 0)
				if b then
					ringbots[#ringbots + 1] = { b = b, p = p, r = rad, a = a }
					S.watch(b.sid, { hp = true })
				end
			end
			for r = 1, n do
				for _, rb in ipairs(ringbots) do
					if not bots.alive(rb.b) then L.respawn(ctx, rb.b, rb.p, { 0, 0, 1 }, 0) end
				end
				ctx:mark('ring', { cfg = 'm67', i = r, k0 = C.kk(), x = { cx = ring.pos[1], cy = ring.pos[2], cz = ring.pos[3] } })
				ctx:prompt('throw', { n = r, of = n }, string.format('M67: throw at the centre of the ring (%d/%d)', r, n))
				local k0 = C.gk
				local function boom()
					for _, d in ipairs(H.dmg_log) do if d.k >= k0 and d.giver == user then return true end end
					return false
				end
				ctx:wait_until(boom, 120, 'boom')
				ctx:wait(20)
			end
			for _, rb in ipairs(ringbots) do
				S.unwatch(rb.b.sid)
				ctx:drop_bot(rb.b)
			end
			return true
		end)
	end
	H.human_watch[user] = nil
	S.unwatch(user)
end

function M.cleanup(ctx)
	H.human_watch = {}
end

return M

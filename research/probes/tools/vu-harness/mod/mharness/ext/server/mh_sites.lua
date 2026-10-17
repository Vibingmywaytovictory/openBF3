-- Recorded poses: wall, range, flat, ring (DESIGN.md sections 6.1-6.9).
-- Camera from the client (NetEvent 'mh:camreq' -> 'mh:cam', server/library/netevents.md) [live:L19];
-- RaycastManager:DetailedRaycast(from, to, maxHits, materialFlags, flags) -> RayCastHit{}
-- (server/library/raycastmanager.md; RayCastFlags shared/type/raycastflags.md) [live:L17]. Emits 'site' rows.
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local chat = require('mh_chat')
local ids = require('mh_ids')

local M = {}

M.sites = {}
M.pending = {}
M.cams = {}      -- sid -> last camera reply {pos, fwd, k, ay, ap}
M.seq = 0

local function flags()
	return RayCastFlags.DontCheckWater + RayCastFlags.DontCheckCharacter + RayCastFlags.DontCheckRagdoll
end

-- ray(a, b): first hit {pos, n} between two points, or nil.
function M.ray(a, b)
	local ok, hits = pcall(function()
		return RaycastManager:DetailedRaycast(V.vec3(a), V.vec3(b), 1, 0, flags())
	end)
	if not ok or type(hits) ~= 'table' or hits[1] == nil then return nil end
	local h = hits[1]
	return { pos = V.v(h.position), n = V.norm(V.v(h.normal)) }
end

function M.ground(p, up, down)
	local h = M.ray({ p[1], p[2] + (up or 2.0), p[3] }, { p[1], p[2] - (down or 6.0), p[3] })
	if h then return h.pos end
	return nil
end

function M.get(name)
	return M.sites[name]
end

function M.invalidate()
	M.sites = {}
end

function M.list()
	local out = {}
	for name, s in pairs(M.sites) do
		out[#out + 1] = string.format('%s %s at %.1f,%.1f,%.1f', name, s.ok and 'ok' or 'BAD', s.pos[1], s.pos[2], s.pos[3])
	end
	table.sort(out)
	return out
end

local function wall(cam)
	local h = M.ray(cam.pos, V.add(cam.pos, V.scale(cam.fwd, 120)))
	if not h then return nil, 'no wall within 120 m' end
	local n = h.n
	if V.dot(n, cam.fwd) > 0 then n = V.scale(n, -1) end
	local d = V.dist(cam.pos, h.pos)
	local inc = V.angle_deg(n, V.scale(cam.fwd, -1))
	if math.abs(n[2]) > 0.3 then return nil, 'surface is not a wall (normal y ' .. string.format('%.2f', n[2]) .. ')' end
	local upv = V.norm(V.sub({ 0, 1, 0 }, V.scale(n, n[2])))
	local right = V.norm(V.cross(upv, n))
	local worst, missing = 0, 0
	for a = -3, 3 do
		for b = -3, 3 do
			local g = V.add(h.pos, V.add(V.scale(right, a * 0.5), V.scale(upv, b * 0.5)))
			local hh = M.ray(V.add(g, V.scale(n, 0.5)), V.sub(g, V.scale(n, 0.5)))
			if hh then
				local dev = math.abs(V.dot(V.sub(hh.pos, h.pos), n))
				if dev > worst then worst = dev end
			else
				missing = missing + 1
			end
		end
	end
	local clear = 0
	for _, dd in ipairs({ 10, 12, 25 }) do
		local p = V.add(h.pos, V.scale(n, dd))
		local gnd = M.ground(p, 2.5, 6.0)
		local blk = M.ray(V.add(h.pos, V.scale(n, 0.05)), p)
		if gnd and not blk then clear = dd end
	end
	local ok = missing == 0 and worst <= 0.02 and clear >= 25 and inc <= 20
	local why = nil
	if missing > 0 then why = 'wall has holes (' .. missing .. '/49)' elseif worst > 0.02 then
		why = string.format('not flat (%.1f cm)', worst * 100)
	elseif clear < 25 then why = 'floor in front not clear to 25 m (' .. clear .. ' m)'
	elseif inc > 20 then why = string.format('look at the wall more squarely (incidence %d deg)', math.floor(inc)) end
	return { pos = h.pos, n = n, dir = upv, d = d, clear = clear, flat = worst, inc = inc, ok = ok }, why
end

local function range(cam)
	local o = M.ground(cam.pos, 0.5, 4.0)
	if not o then return nil, 'no ground below the camera' end
	local u = V.horiz(cam.fwd)
	local a = V.add(o, { 0, 1.3, 0 })
	local h = M.ray(a, V.add(a, V.scale(u, 200)))
	local L = h and V.dist(a, h.pos) or 200
	local ok = L >= 30
	return { pos = o, dir = u, clear = L, ok = ok }, (not ok) and string.format('only %.0f m clear', L) or nil
end

local function flat(cam)
	local p = M.ground(cam.pos, 0.5, 4.0)
	if not p then return nil, 'no ground below the camera' end
	local u = V.horiz(cam.fwd)
	local L = 200
	for _, hgt in ipairs({ 0.5, 1.5 }) do
		local a = V.add(p, { 0, hgt, 0 })
		local h = M.ray(a, V.add(a, V.scale(u, 40)))
		local l = h and V.dist(a, h.pos) or 40
		if l < L then L = l end
	end
	local worst = 0
	for s = 2, 30, 2 do
		local g = M.ground(V.add(p, V.scale(u, s)), 1.0, 2.0)
		local dev = g and math.abs(g[2] - p[2]) or 1.0
		if dev > worst then worst = dev end
	end
	local top = M.ray(V.add(p, { 0, 2, 0 }), V.add(p, { 0, 62, 0 }))
	local up = top and (top.pos[2] - p[2]) or 60
	local ok = L >= 30 and worst <= 0.05
	local why = nil
	if L < 30 then why = string.format('only %.0f m clear', L) elseif worst > 0.05 then
		why = string.format('not flat (%.0f cm)', worst * 100)
	end
	return { pos = p, dir = u, clear = L, flat = worst, up = up, ok = ok }, why
end

local function ring(cam)
	local h = M.ray(cam.pos, V.add(cam.pos, V.scale(cam.fwd, 80)))
	if not h then return nil, 'look at the ground within 80 m' end
	return { pos = h.pos, ok = true }, nil
end

local BUILD = { wall = wall, range = range, flat = flat, ring = ring }

-- request(name, player, args, cb): asks the player's client for its camera; cb(site|nil, why) on reply or timeout.
function M.request(name, pl, args, cb)
	local sid = ids.player(pl)
	M.seq = M.seq + 1
	M.pending[sid] = { name = name, args = args, cb = cb, k = C.gk, tag = M.seq, pl = pl }
	NetEvents:SendTo('mh:camreq', pl, M.seq)
end

-- on_cam(player, msg): reply {tag, p={x,y,z}, f={x,y,z}, sgn, ay, ap} from the client (mh_cam).
function M.on_cam(pl, msg)
	local sid = ids.player(pl)
	if type(msg) ~= 'table' or type(msg.p) ~= 'table' or type(msg.f) ~= 'table' then return end
	local cam = { pos = { msg.p[1], msg.p[2], msg.p[3] }, fwd = V.norm({ msg.f[1], msg.f[2], msg.f[3] }), k = C.gk,
		ay = msg.ay, ap = msg.ap, sgn = msg.sgn }
	M.cams[sid] = cam
	local req = M.pending[sid]
	if req == nil or req.tag ~= msg.tag then return end
	M.pending[sid] = nil
	if req.name == 'cam' then
		req.cb(cam)
		return
	end
	local s, why = BUILD[req.name](cam)
	if s then
		M.sites[req.name] = s
		R.row('site', { name = req.name, pos = s.pos, ok = s.ok == true, n = s.n, dir = s.dir, d = s.d, clear = s.clear,
			flat = s.flat, inc = s.inc, up = s.up })
	end
	if req.cb then req.cb(s, why) end
end

function M.tick()
	for sid, req in pairs(M.pending) do
		if C.gk - req.k > 3 * C.hzv then
			M.pending[sid] = nil
			if req.cb then req.cb(nil, 'no camera reply from the client (is the mod client script loaded?)') end
		end
	end
end

-- describe(name, site, why) -> one chat line.
function M.describe(name, s, why)
	if s == nil then return name .. ' failed: ' .. tostring(why) end
	if name == 'wall' then
		return string.format('wall %s: %.1f m, incidence %d deg, flat %.1f cm, floor clear %d m%s', s.ok and 'ok' or 'BAD',
			s.d, math.floor(s.inc + 0.5), s.flat * 100, s.clear, why and (' - ' .. why) or '')
	elseif name == 'range' then
		return string.format('range %s: %.0f m clear%s', s.ok and 'ok' or 'BAD', s.clear, why and (' - ' .. why) or '')
	elseif name == 'flat' then
		return string.format('flat %s: %.0f m clear, flat %.0f cm, sky %.0f m%s', s.ok and 'ok' or 'BAD', s.clear,
			s.flat * 100, s.up, why and (' - ' .. why) or '')
	end
	return name .. ' ok'
end

return M

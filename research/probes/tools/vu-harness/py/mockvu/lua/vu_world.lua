-- Mock world (DESIGN.md 13.2): level layout from levels.py (ground plane, wall/backstop rectangles, flags), static
-- raycasts, soldier cylinders with hit zones, the entity list for iterators, materials, a seeded PRNG.
mock = mock or {}
local M = mock
local A = M.A

local W = {}
M.W = W

-- splitmix64 PRNG streams (independent of math.random, which the mod environment shares).
function M.rng(seed)
	local s = math.tointeger(seed) or 1
	local r = {}
	function r.u()
		s = s + 0x9E3779B97F4A7C15
		local z = s
		z = (z ~ (z >> 30)) * 0xBF58476D1CE4E5B9
		z = (z ~ (z >> 27)) * 0x94D049BB133111EB
		z = z ~ (z >> 31)
		return ((z >> 11) & 0x1FFFFFFFFFFFFF) / 9007199254740992.0
	end
	function r.n()
		local u1, u2 = r.u(), r.u()
		if u1 < 1e-300 then u1 = 1e-300 end
		return math.sqrt(-2 * math.log(u1)) * math.cos(2 * math.pi * u2)
	end
	function r.range(a, b) return a + (b - a) * r.u() end
	return r
end

W.planes = {}
W.rects = {}
W.ents = {}        -- every engine entity (raw), in creation order
W.next_uid = 7000
W.mats = {}

local function material(name, fai, pmi, ppi)
	local o = { __cls = 'MaterialContainerPair', __owner = 'none', flagsAndIndex = fai, physicsMaterialIndex = pmi,
		physicsPropertyIndex = ppi, __dtype = 'MaterialContainerPair', mname = name }
	o.__get = { typeInfo = function() return M.typeinfo('MaterialContainerPair') end }
	return o
end

function W.uid()
	W.next_uid = W.next_uid + 1
	return W.next_uid
end

function W.add_ent(o)
	o.uniqueId = o.uniqueId or W.uid()
	M.next_iid = M.next_iid + 1
	o.instanceId = o.instanceId or M.next_iid
	W.ents[#W.ents + 1] = o
	return o
end

function W.remove_ent(o)
	for i, e in ipairs(W.ents) do
		if e == o then
			table.remove(W.ents, i)
			return
		end
	end
end

local STATIC_GET = {
	typeInfo = function(o) return M.typeinfo(o.__etype) end,
	transform = function(o) return M.lt(M.vec3(1, 0, 0), M.vec3(0, 1, 0), M.vec3(0, 0, 1), M.a3v(o.c or { 0, 0, 0 })) end,
}

-- reset(layout): layout tables come from levels.py (already offset by the origin).
function W.reset(L)
	W.L = L
	W.planes, W.rects = {}, {}
	W.mats = { ground = material('ground', 0x0101, 1, 3), wall = material('concrete', 0x0204, 2, 5),
		metal = material('metal', 0x0307, 3, 7) }
	local keep = {}
	for _, e in ipairs(W.ents) do if e.__etype == 'ServerSoldierEntity' then keep[#keep + 1] = e end end
	W.ents = keep
	for _, p in ipairs(L.planes or {}) do
		local st = W.add_ent({ __cls = 'GameEntity', __etype = 'StaticModelEntity', __owner = 'none', c = A.copy(p.p),
			__get = STATIC_GET })
		W.planes[#W.planes + 1] = { p = A.copy(p.p), n = A.norm(p.n), mat = W.mats[p.mat or 'ground'], ent = st }
	end
	for _, r in ipairs(L.rects or {}) do
		local st = W.add_ent({ __cls = 'GameEntity', __etype = 'StaticModelEntity', __owner = 'none', c = A.copy(r.c),
			__get = STATIC_GET })
		W.rects[#W.rects + 1] = { name = r.name, c = A.copy(r.c), n = A.norm(r.n), u = A.norm(r.u), v = A.norm(r.v),
			hu = r.hu, hv = r.hv, mat = W.mats[r.mat or 'wall'], ent = st }
	end
end

function W.ground_y(p)
	-- highest ground plane below p (planes are horizontal in the layouts)
	local best = nil
	for _, pl in ipairs(W.planes) do
		if pl.n[2] > 0.99 and pl.p[2] <= p[2] + 0.5 and (best == nil or pl.p[2] > best) then best = pl.p[2] end
	end
	return best or 0.0
end

-- static hits along the segment a -> b: list of {s, pos, n, mat, ent}
local function static_hits(a, b, out)
	local d = A.sub(b, a)
	for _, pl in ipairs(W.planes) do
		local den = A.dot(d, pl.n)
		if math.abs(den) > 1e-12 then
			local s = A.dot(A.sub(pl.p, a), pl.n) / den
			if s >= 0 and s <= 1 then
				local n = pl.n
				if den > 0 then n = A.scale(n, -1) end
				out[#out + 1] = { s = s, pos = A.add(a, A.scale(d, s)), n = n, mat = pl.mat, ent = pl.ent }
			end
		end
	end
	for _, r in ipairs(W.rects) do
		local den = A.dot(d, r.n)
		if math.abs(den) > 1e-12 then
			local s = A.dot(A.sub(r.c, a), r.n) / den
			if s >= 0 and s <= 1 then
				local x = A.add(a, A.scale(d, s))
				local rel = A.sub(x, r.c)
				if math.abs(A.dot(rel, r.u)) <= r.hu and math.abs(A.dot(rel, r.v)) <= r.hv then
					local n = r.n
					if den > 0 then n = A.scale(n, -1) end
					out[#out + 1] = { s = s, pos = x, n = n, mat = r.mat, ent = r.ent, rect = r.name }
				end
			end
		end
	end
	return out
end
W.static_hits = static_hits

-- Soldier cylinder (feet p, radius R, height H): first intersection of segment a -> b; returns s, point, normal, b
-- (impact parameter: horizontal distance of the line from the axis), yrel (height above the feet).
W.RADIUS = 0.35
W.HEIGHT = { [0] = 1.85, [1] = 1.3, [2] = 0.5 }

function W.cyl_hit(a, b, p, H)
	local R = W.RADIUS
	local dx, dz = b[1] - a[1], b[3] - a[3]
	local fx, fz = a[1] - p[1], a[3] - p[3]
	local qa = dx * dx + dz * dz
	local best = nil
	local bimp = 0
	if qa > 1e-12 then
		-- impact parameter of the infinite horizontal line
		local cr = math.abs(fx * dz - fz * dx) / math.sqrt(qa)
		bimp = cr
		local qb = 2 * (fx * dx + fz * dz)
		local qc = fx * fx + fz * fz - R * R
		local disc = qb * qb - 4 * qa * qc
		if disc >= 0 then
			local sq = math.sqrt(disc)
			local s1 = (-qb - sq) / (2 * qa)
			if s1 >= 0 and s1 <= 1 then
				local y = a[2] + (b[2] - a[2]) * s1
				if y >= p[2] and y <= p[2] + H then
					local x = { a[1] + dx * s1, y, a[3] + dz * s1 }
					best = { s = s1, pos = x, n = A.norm({ x[1] - p[1], 0, x[3] - p[3] }), b = cr, yrel = y - p[2] }
				end
			end
		end
	end
	-- top cap
	local dy = b[2] - a[2]
	if math.abs(dy) > 1e-12 then
		local top = p[2] + H
		local s = (top - a[2]) / dy
		if s >= 0 and s <= 1 and (best == nil or s < best.s) then
			local x = A.add(a, A.scale(A.sub(b, a), s))
			local rx, rz = x[1] - p[1], x[3] - p[3]
			if rx * rx + rz * rz <= R * R then
				best = { s = s, pos = x, n = { 0, 1, 0 }, b = math.sqrt(rx * rx + rz * rz), yrel = H }
			end
		end
	end
	if best then best.b = math.min(best.b, bimp > 0 and bimp or best.b) end
	return best
end

-- zone(H, yrel, b) -> bone index, zone name, multiplier (DESIGN 13.2 [mock]; tests must not depend on exact values)
W.MULT = { head = 2.0, chest = 1.0, arm = 0.9, stomach = 1.0, legs = 0.9 }
W.BONE = { head = 1, chest = 2, arm = 3, stomach = 4, legs = 5 }
function W.zone(H, yrel, b)
	local f = yrel / H
	local z
	if f >= 0.87 then z = 'head'
	elseif f >= 0.55 then z = (b >= 0.25) and 'arm' or 'chest'
	elseif f >= 0.45 then z = 'stomach'
	else z = 'legs' end
	return W.BONE[z], z, W.MULT[z]
end

-- raycast(a, b, max, chars): hits sorted by distance; chars = soldiers included (list of soldier raws to test).
function W.raycast(a, b, maxhits, chars)
	local out = static_hits(a, b, {})
	if chars then
		for _, so in ipairs(chars) do
			local h = W.cyl_hit(a, b, so.p, W.HEIGHT[so.pose] or 1.85)
			if h then
				h.so = so
				h.mat = W.mats.metal
				out[#out + 1] = h
			end
		end
	end
	table.sort(out, function(x, y) return x.s < y.s end)
	local res = {}
	for i = 1, math.min(#out, maxhits or 1) do res[i] = out[i] end
	return res
end

-- Mock VU value types (DESIGN.md 13.2): Vec2, Vec3, LinearTransform. Plain Lua tables with metatables; unknown members
-- raise like every other mock type. Runs in the host environment (outside the mod sandbox).
mock = mock or {}
local M = mock

local sqrt = math.sqrt

local V3 = {}
local V3MT = {}
V3MT.__index = function(v, k)
	if k == 'magnitude' then return sqrt(rawget(v, 'x') ^ 2 + rawget(v, 'y') ^ 2 + rawget(v, 'z') ^ 2) end
	return M.missing('Vec3', k)
end
V3MT.__newindex = function(v, k, val)
	if k == 'x' or k == 'y' or k == 'z' then
		if type(val) ~= 'number' then error('mock: Vec3.' .. k .. ' needs a number', 2) end
		rawset(v, k, val)
		return
	end
	error('mock: Vec3.' .. tostring(k) .. ' not writable', 2)
end
V3MT.__add = function(a, b) return M.vec3(a.x + b.x, a.y + b.y, a.z + b.z) end
V3MT.__sub = function(a, b) return M.vec3(a.x - b.x, a.y - b.y, a.z - b.z) end
V3MT.__mul = function(a, b)
	if type(a) == 'number' then return M.vec3(a * b.x, a * b.y, a * b.z) end
	return M.vec3(a.x * b, a.y * b, a.z * b)
end
V3MT.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
V3MT.__tostring = function(v) return string.format('Vec3(%g, %g, %g)', v.x, v.y, v.z) end
V3MT.__name = 'Vec3'

function M.vec3(x, y, z)
	if type(x) ~= 'number' or type(y) ~= 'number' or type(z) ~= 'number' then
		error('mock: Vec3(x, y, z) needs three numbers', 2)
	end
	return setmetatable({ x = x, y = y, z = z }, V3MT)
end
function M.is_vec3(v) return getmetatable(v) == V3MT end
function M.v3a(v) return { v.x, v.y, v.z } end
function M.a3v(a) return M.vec3(a[1], a[2], a[3]) end

local V2MT = {}
V2MT.__index = function(v, k)
	if k == 'magnitude' then return sqrt(rawget(v, 'x') ^ 2 + rawget(v, 'y') ^ 2) end
	return M.missing('Vec2', k)
end
V2MT.__newindex = function(v, k, val)
	if k == 'x' or k == 'y' then rawset(v, k, val) return end
	error('mock: Vec2.' .. tostring(k) .. ' not writable', 2)
end
V2MT.__name = 'Vec2'
function M.vec2(x, y) return setmetatable({ x = x, y = y }, V2MT) end

local LTMT = {}
local LT_FIELDS = { left = true, up = true, forward = true, trans = true }
LTMT.__index = function(t, k) return M.missing('LinearTransform', k) end
LTMT.__newindex = function(t, k, v)
	if LT_FIELDS[k] then
		if not M.is_vec3(v) then error('mock: LinearTransform.' .. k .. ' needs a Vec3', 2) end
		rawset(t, k, M.vec3(v.x, v.y, v.z))
		return
	end
	error('mock: LinearTransform.' .. tostring(k) .. ' not in api_inventory', 2)
end
LTMT.__name = 'LinearTransform'

function M.lt(left, up, forward, trans)
	if left == nil then
		left, up, forward, trans = M.vec3(1, 0, 0), M.vec3(0, 1, 0), M.vec3(0, 0, 1), M.vec3(0, 0, 0)
	end
	for _, v in ipairs({ left, up, forward, trans }) do
		if not M.is_vec3(v) then error('mock: LinearTransform(left, up, forward, trans) needs four Vec3', 2) end
	end
	return setmetatable({ left = M.vec3(left.x, left.y, left.z), up = M.vec3(up.x, up.y, up.z),
		forward = M.vec3(forward.x, forward.y, forward.z), trans = M.vec3(trans.x, trans.y, trans.z) }, LTMT)
end
function M.is_lt(t) return getmetatable(t) == LTMT end

-- plain array math for the mock's own code
local A = {}
M.A = A
function A.add(a, b) return { a[1] + b[1], a[2] + b[2], a[3] + b[3] } end
function A.sub(a, b) return { a[1] - b[1], a[2] - b[2], a[3] - b[3] } end
function A.scale(a, s) return { a[1] * s, a[2] * s, a[3] * s } end
function A.dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
function A.len(a) return sqrt(A.dot(a, a)) end
function A.norm(a)
	local l = A.len(a)
	if l < 1e-12 then return { 0, 0, 0 } end
	return { a[1] / l, a[2] / l, a[3] / l }
end
function A.cross(a, b) return { a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1] } end
function A.dist(a, b) return A.len(A.sub(a, b)) end
function A.copy(a) return { a[1], a[2], a[3] } end
-- basis of a direction: right and up perpendicular to d (up near +y)
function A.basis(d)
	local up = { 0, 1, 0 }
	local r = A.cross(d, up)
	if A.len(r) < 1e-6 then r = { 1, 0, 0 } end
	r = A.norm(r)
	local u = A.norm(A.cross(r, d))
	return r, u
end
-- rotate direction d by small angles (deg) yaw (about up) and pitch (about right)
function A.deviate(d, yaw_deg, pitch_deg)
	local r, u = A.basis(d)
	local ty, tp = math.tan(math.rad(yaw_deg)), math.tan(math.rad(pitch_deg))
	return A.norm(A.add(d, A.add(A.scale(r, ty), A.scale(u, tp))))
end

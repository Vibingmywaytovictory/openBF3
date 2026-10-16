-- Plain-number vector math (DESIGN.md section 3.9). Vectors are Lua arrays {x, y, z}; VU operator overloads are not
-- used. Vec3(x, y, z) (shared/type/vec3.md) is built only to pass values to the API; Vec3.x/y/z are read to convert.
local M = {}

local sqrt, atan, asin, pi = math.sqrt, math.atan, math.asin, math.pi

function M.v(vec)
	if vec == nil then return nil end
	return { vec.x, vec.y, vec.z }
end

function M.vec3(a)
	return Vec3(a[1], a[2], a[3])
end

function M.add(a, b) return { a[1] + b[1], a[2] + b[2], a[3] + b[3] } end
function M.sub(a, b) return { a[1] - b[1], a[2] - b[2], a[3] - b[3] } end
function M.scale(a, s) return { a[1] * s, a[2] * s, a[3] * s } end
function M.dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
function M.len(a) return sqrt(M.dot(a, a)) end
function M.cross(a, b)
	return { a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1] }
end
function M.norm(a)
	local l = M.len(a)
	if l < 1e-12 then return { 0, 0, 0 } end
	return { a[1] / l, a[2] / l, a[3] / l }
end
function M.horiz(a)
	return M.norm({ a[1], 0, a[3] })
end
function M.dist(a, b) return M.len(M.sub(a, b)) end
function M.lerp(a, b, s) return M.add(a, M.scale(M.sub(b, a), s)) end

-- Rotate a horizontal direction by angle r (radians) about +y.
function M.yrot(a, r)
	local c, s = math.cos(r), math.sin(r)
	return { a[1] * c + a[3] * s, a[2], -a[1] * s + a[3] * c }
end

function M.angle_deg(a, b)
	local c = M.dot(M.norm(a), M.norm(b))
	if c > 1 then c = 1 elseif c < -1 then c = -1 end
	return math.deg(math.acos(c))
end

local function wrap(r)
	r = r % (2 * pi)
	if r < 0 then r = r + 2 * pi end
	return r
end
M.wrap = wrap

-- Aim convention (DESIGN.md 3.9, live:L08): cal = {sx, sz, off, unit ('rad'|'deg'), ps}.
-- yaw_input = (atan2(sx*x, sz*z) + off) in unit; pitch_input = ps * asin(y) in unit.
function M.dir_to_aim(cal, d)
	local n = M.norm(d)
	local yaw = wrap(atan(cal.sx * n[1], cal.sz * n[3]) + cal.off)
	local p = asin(math.max(-1, math.min(1, n[2]))) * cal.ps
	if cal.unit == 'deg' then return math.deg(yaw), math.deg(p) end
	return yaw, p
end

-- Inverse: input (yaw, pitch) -> world direction under cal.
function M.aim_to_dir(cal, yaw, pitch)
	if cal.unit == 'deg' then yaw, pitch = math.rad(yaw), math.rad(pitch) end
	local a = yaw - cal.off
	local p = pitch * cal.ps
	local cp = math.cos(p)
	-- atan2(sx*x, sz*z) = a  ->  sx*x = sin(a)*r, sz*z = cos(a)*r
	return { cal.sx * math.sin(a) * cp, math.sin(p), cal.sz * math.cos(a) * cp }
end

-- Every candidate convention (8 reflections/rotations x 2 units x 2 pitch signs).
function M.candidates()
	local out = {}
	for _, unit in ipairs({ 'rad', 'deg' }) do
		for _, sx in ipairs({ 1, -1 }) do
			for _, sz in ipairs({ 1, -1 }) do
				for n = 0, 3 do
					for _, ps in ipairs({ 1, -1 }) do
						out[#out + 1] = { sx = sx, sz = sz, off = n * pi / 2, unit = unit, ps = ps }
					end
				end
			end
		end
	end
	return out
end

-- Ray / plane intersection: origin o, direction d, plane point p, normal n -> point or nil.
function M.ray_plane(o, d, p, n)
	local den = M.dot(d, n)
	if math.abs(den) < 1e-9 then return nil end
	local s = M.dot(M.sub(p, o), n) / den
	if s < 0 then return nil end
	return M.add(o, M.scale(d, s))
end

return M

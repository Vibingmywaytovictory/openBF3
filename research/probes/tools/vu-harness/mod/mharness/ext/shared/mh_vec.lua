-- Plain-number vector math (SKELETON; DESIGN.md section 3.9). No VU operator overloads are used;
-- Vec3(x, y, z) (shared/type/vec3.md) is built only to pass values to the API.
local M = {}

function M.sub(a, b) return { a[1] - b[1], a[2] - b[2], a[3] - b[3] } end
function M.dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
function M.len(a) return math.sqrt(M.dot(a, a)) end

return M

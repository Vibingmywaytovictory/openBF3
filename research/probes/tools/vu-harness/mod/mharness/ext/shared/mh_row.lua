-- Row builder (SKELETON; DESIGN.md sections 3.5, 3.6, 4).
-- The only module that may call print(). Emits 'MH1 <json>' with the envelope v, run, sc, r, k, t, ms, q, e.
-- Own deterministic JSON encoder: %d integers; floats %.9g by default (positions are site-relative, DESIGN 4.1);
-- %.6g only for fields the schema marks 'q6'; Vec3 -> [x,y,z], non-finite -> null (lim.nf).
-- Size cap 1000 B (oversize -> 'trunc' row); token bucket 240 rows/s, burst 480; priority-2 rows dropped first.
local M = {}

M.VERSION = '0.1.0'
M.MAX_BYTES = 1000

-- row(e, payload, prio): prio 1 is never dropped, prio 2 may be dropped by the limiter.
function M.row(e, payload, prio)
end

-- safe(where, fn, ...): pcall wrapper; on error emits an 'err' row and counts; disables a handler after 20 errors.
function M.safe(where, fn, ...)
	return pcall(fn, ...)
end

-- encode(value): compact JSON of numbers, booleans, labels, arrays and small objects.
function M.encode(value)
	return 'null'
end

return M

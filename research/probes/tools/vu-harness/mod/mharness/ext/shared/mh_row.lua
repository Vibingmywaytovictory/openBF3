-- Row builder (DESIGN.md sections 3.5, 3.6, 4). The only module that may call print().
-- Emits 'MH1 <json>' with the envelope v, run, sc, r, k, t, ms, q, e. VU prefixes every print with
-- '[<time>] [info] [VeniceEXT] [mharness]' (R section 3.3) [live:L01].
-- Own deterministic JSON encoder: integers with %d, floats with %.9g (pass 3), arrays for sequences, objects with sorted
-- keys; non-finite numbers become null and count in lim.nf. Vec3 values are converted by mh_vec before they get here.
-- Size cap 1000 B (oversize -> 'trunc' row); token bucket 240 rows/s, burst 480; priority-2 rows are dropped first.
-- SharedUtils:GetTimeMS (shared/library/sharedutils.md).
local M = {}

M.VERSION = '0.2.0'
M.MAX_BYTES = 1000
M.RATE = 240
M.BURST = 480
M.realm = 'S'
M.clock = nil -- set by the realm entry (mh_clock)
M.seq = 0
M.stats = { rows = 0, nf = 0, over = 0, errs = 0, drop = {}, dropped = 0 }
M.disabled = {}
M.err_counts = {}
M.on_disable = nil -- callback(where) set by the server entry (chat notice)

local PRIO2 = { gs = true, wf = true, hp = true, tk = true, cp = true, veh = true, fov = true }
local tokens = 480
local last_ms = nil
local drop_window = {}
local drop_any = false
local drop_ms = 0

local fmt = string.format
local mtype = math.type
local huge = math.huge

-- Labels: [A-Za-z0-9_.:/+=#()[], -]{0,96} (schema label_pattern).
function M.label(s)
	s = tostring(s)
	s = s:gsub('[^A-Za-z0-9_%.:/%+=#%(%)%[%], %-]', '_')
	if #s > 96 then s = s:sub(1, 96) end
	return s
end

local function enc_str(s)
	s = s:gsub('[%c"\\]', function(c)
		if c == '"' then return '\\"' end
		if c == '\\' then return '\\\\' end
		return fmt('\\u%04x', c:byte())
	end)
	return '"' .. s .. '"'
end

local function enc_num(v)
	if mtype(v) == 'integer' then return fmt('%d', v) end
	if v ~= v or v == huge or v == -huge then
		M.stats.nf = M.stats.nf + 1
		return 'null'
	end
	local s = fmt('%.9g', v)
	if s == '-0' then s = '0' end
	return s
end

M.EMPTY = setmetatable({}, { __name = 'mh_empty_array' })

local enc
enc = function(v, depth)
	local tv = type(v)
	if tv == 'number' then return enc_num(v) end
	if tv == 'boolean' then return v and 'true' or 'false' end
	if tv == 'string' then return enc_str(v) end
	if tv == 'nil' then return 'null' end
	if tv ~= 'table' then return enc_str(tostring(v)) end
	if depth > 5 then return 'null' end
	if v == M.EMPTY then return '[]' end
	if v[1] ~= nil then
		local parts = {}
		for i = 1, #v do parts[i] = enc(v[i], depth + 1) end
		return '[' .. table.concat(parts, ',') .. ']'
	end
	local keys = {}
	for key in pairs(v) do keys[#keys + 1] = tostring(key) end
	table.sort(keys)
	local parts = {}
	for i = 1, #keys do
		local key = keys[i]
		local val = v[key]
		if val == nil then val = v[tonumber(key)] end
		parts[#parts + 1] = enc_str(key) .. ':' .. enc(val, depth + 1)
	end
	return '{' .. table.concat(parts, ',') .. '}'
end

function M.encode(value)
	return enc(value, 0)
end

local function now_ms()
	local ok, ms = pcall(function() return SharedUtils:GetTimeMS() end)
	if ok and type(ms) == 'number' then return ms end
	return 0
end

local function admit(e, prio, ms)
	if last_ms == nil then last_ms = ms end
	local dms = ms - last_ms
	if dms > 0 then
		tokens = math.min(M.BURST, tokens + M.RATE * dms / 1000)
		last_ms = ms
	end
	if tokens >= 1 then
		tokens = tokens - 1
		return true
	end
	if prio == 1 then return true end
	drop_window[e] = (drop_window[e] or 0) + 1
	M.stats.dropped = M.stats.dropped + 1
	drop_any = true
	return false
end

local function envelope(e)
	local c = M.clock
	local run, sc, k, t = 'none', 'S', 0, 0.0
	if c then run, sc, k, t = c.run_id(), c.scen(), c.kk(), c.tt() end
	M.seq = M.seq + 1
	return fmt('{"v":1,"run":%s,"sc":%s,"r":%s,"k":%d,"t":%s,"ms":%d,"q":%d,"e":%s',
		enc_str(run), enc_str(sc), enc_str(M.realm), k, enc_num(t + 0.0), math.floor(now_ms()), M.seq, enc_str(e))
end

local function emit_line(e, payload)
	local head = envelope(e)
	local body = enc(payload or {}, 0)
	local line
	if body == '{}' then line = head .. '}' else line = head .. ',' .. body:sub(2) end
	if #line > M.MAX_BYTES then
		M.stats.over = M.stats.over + 1
		local len = #line
		head = envelope('trunc')
		line = head .. ',' .. fmt('"len":%d,"of":%s}', len, enc_str(e))
	end
	print('MH1 ' .. line)
	M.stats.rows = M.stats.rows + 1
end

local function flush_drops(ms)
	if drop_any and ms - drop_ms >= 1000 then
		local d = drop_window
		drop_window = {}
		drop_any = false
		drop_ms = ms
		emit_line('lim', { drop = d, nf = M.stats.nf, over = M.stats.over })
	end
end

-- row(e, payload, prio): prio 1 is never dropped; prio 2 (default for gs wf hp tk cp veh fov) may be dropped.
function M.row(e, payload, prio)
	prio = prio or (PRIO2[e] and 2 or 1)
	local ms = now_ms()
	flush_drops(ms)
	if not admit(e, prio, ms) then return false end
	emit_line(e, payload)
	return true
end

-- plain(text): a log line that is not a row (greeting, join line). Never parsed by the extractor.
function M.plain(text)
	print('mharness: ' .. tostring(text))
end

local function errmsg(x)
	local s = M.label(tostring(x))
	if #s > 120 then s = s:sub(1, 120) end
	return s
end

-- safe(where, fn, ...): pcall wrapper; on error emits an 'err' row and counts; disables a handler after 20 errors.
function M.safe(where, fn, ...)
	if M.disabled[where] then return false end
	local res = table.pack(pcall(fn, ...))
	if not res[1] then
		local n = (M.err_counts[where] or 0) + 1
		M.err_counts[where] = n
		M.stats.errs = M.stats.errs + 1
		M.row('err', { where = M.label(where), msg = errmsg(res[2]), n = n })
		if n >= 20 then
			M.disabled[where] = true
			if M.on_disable then pcall(M.on_disable, where) end
		end
		return false, res[2]
	end
	return table.unpack(res, 1, res.n)
end

return M

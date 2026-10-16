-- Mock VU core (DESIGN.md 13.2): inventory-driven object model, the write/fixture/RCON journal, Events, Hooks,
-- NetEvents, enums, json, the clock and the mod sandbox. Runs in the host environment of one realm's lupa runtime.
-- Every member the mod reads or writes is checked against schema/api_inventory.json along the inherits chain;
-- unknown members raise 'mock: <Type>.<name> not in api_inventory' (or return nil with member_missing=nil).
mock = mock or {}
local M = mock

M.realm = 'server'
M.k = 0
M.hz = 30
M.dt = 1 / 30
M.t = 0.0
M.ms0 = 1000000
M.wall0 = 0       -- seconds of the mock wall clock at k = 0 (2026-10-16 21:00:00 = 75600 s into the day)
M.journal = {}    -- {k, kind='write'|'call'|'rcon'|'hook', view, field, owner, sid, cmd, nargs}
M.out = {}        -- printed lines {k, text}
M.errors = {}     -- handler errors (an uncaught error in the mod is a test failure)
M.missing_log = {}
M.laws = {}
M.member_missing = 'raise'

-- Inventory ---------------------------------------------------------------------------------------------------------
local PROPS, METHODS, INH = {}, {}, {}
M.ENUMS = {}
M.BOT_ASSIGN = {}
M.FIXTURE = {}
M.RCON_READ, M.RCON_ADMIN = {}, {}

function M.load_inventory(inv)
	for _, e in ipairs(inv.entries) do
		local kind, sym, realm = e.kind, e.sym, e.realm
		if kind == 'prop' then
			local t, f = sym:match('^([%w_]+)%.([%w_]+)$')
			PROPS[t .. '@' .. realm] = PROPS[t .. '@' .. realm] or {}
			PROPS[t .. '@' .. realm][f] = e.ro == true
		elseif kind == 'method' then
			local t, m = sym:match('^([%w_]+):([%w_]+)$')
			METHODS[t .. '@' .. realm] = METHODS[t .. '@' .. realm] or {}
			METHODS[t .. '@' .. realm][m] = true
		elseif kind == 'enum' then
			local t, v = sym:match('^([%w_]+)%.([%w_]+)$')
			M.ENUMS[t] = M.ENUMS[t] or {}
			M.ENUMS[t][v] = math.tointeger(e.value) or e.value
		end
	end
	for key, parent in pairs(inv.inherits) do
		if key ~= 'doc' then INH[key] = parent end
	end
	for _, f in ipairs(inv.bot_assign.fields) do M.BOT_ASSIGN[f] = true end
	for _, f in ipairs(inv.fixture_calls) do M.FIXTURE[f] = true end
	for _, c in ipairs(inv.rcon_read_whitelist.read) do M.RCON_READ[c] = true end
	for _, c in ipairs(inv.rcon_read_whitelist.admin) do M.RCON_ADMIN[c] = true end
end

local MEMBER_CACHE = {}

-- members(view): {name = 'ro'|'rw'|'m'} along the inherits chain for the current realm.
local function members(view)
	local c = MEMBER_CACHE[view]
	if c then return c end
	c = {}
	local t = view
	local guard = 0
	while t and guard < 20 do
		guard = guard + 1
		for _, r in ipairs({ M.realm, 'shared' }) do
			local p = PROPS[t .. '@' .. r]
			if p then for f, ro in pairs(p) do if c[f] == nil then c[f] = ro and 'ro' or 'rw' end end end
			local m = METHODS[t .. '@' .. r]
			if m then for f in pairs(m) do if c[f] == nil then c[f] = 'm' end end end
		end
		t = INH[t .. '@' .. M.realm] or INH[t .. '@shared']
	end
	MEMBER_CACHE[view] = c
	return c
end
M.members = members

function M.chain(view)
	local out = {}
	local t = view
	local guard = 0
	while t and guard < 20 do
		guard = guard + 1
		out[#out + 1] = t
		t = INH[t .. '@' .. M.realm] or INH[t .. '@shared']
	end
	return out
end

function M.missing(view, k)
	if M.member_missing == 'nil' then
		M.missing_log[#M.missing_log + 1] = view .. '.' .. tostring(k)
		return nil
	end
	error('mock: ' .. view .. '.' .. tostring(k) .. ' not in api_inventory', 3)
end

-- Proxies -----------------------------------------------------------------------------------------------------------
-- raw objects are plain tables owned by the mock; the mod only ever sees proxies. raw.__owner: 'bot' | 'human' |
-- 'input' | 'none'; raw.__sid: the bot's small id (from its name mhNNN) for the fx cross-check.
local P = setmetatable({}, { __mode = 'k' })
M.P = P
M.IMPL = {}  -- view -> {method = fn(raw, ...)}

local function impl_for(view, k)
	for _, t in ipairs(M.chain(view)) do
		local i = M.IMPL[t]
		if i and i[k] then return i[k] end
	end
	return nil
end

local function journal(e)
	e.k = M.k
	M.journal[#M.journal + 1] = e
end
M.jot = journal

local PMT = {}
PMT.__index = function(p, k)
	local e = P[p]
	local o, view = e.o, e.v
	local m = members(view)[k]
	if m == nil then return M.missing(view, k) end
	if m == 'm' then
		local fn = impl_for(view, k)
		if fn == nil then error('mock: ' .. view .. ':' .. k .. ' not implemented by the mock', 2) end
		return function(self, ...)
			local se = P[self]
			if se == nil then error('mock: ' .. view .. ':' .. k .. ' called without self (use :)', 2) end
			if M.FIXTURE[k] then journal({ kind = 'call', view = view, field = k, owner = se.o.__owner, sid = se.o.__sid }) end
			return fn(se.o, ...)
		end
	end
	local g = o.__get and o.__get[k]
	if g then return g(o) end
	return o[k]
end
PMT.__newindex = function(p, k, v)
	local e = P[p]
	local o, view = e.o, e.v
	local m = members(view)[k]
	if m == nil then error('mock: ' .. view .. '.' .. tostring(k) .. ' not in api_inventory', 2) end
	if m == 'm' then error('mock: cannot assign method ' .. view .. ':' .. k, 2) end
	if m == 'ro' then error('mock: ' .. view .. '.' .. k .. ' is read-only', 2) end
	journal({ kind = 'write', view = view, field = k, owner = o.__owner, sid = o.__sid })
	local s = o.__set and o.__set[k]
	if s then s(o, v) else o[k] = v end
end
PMT.__tostring = function(p)
	local e = P[p]
	if e.o.__tostring then return e.o.__tostring(e.o) end
	return 'mock:' .. e.v
end
PMT.__name = 'VUObject'

function M.px(o, view)
	if o == nil then return nil end
	o.__px = o.__px or {}
	local p = o.__px[view]
	if p then return p end
	p = setmetatable({}, PMT)
	P[p] = { o = o, v = view }
	o.__px[view] = p
	return p
end

function M.raw(p)
	local e = P[p]
	return e and e.o or nil
end

function M.view(p)
	local e = P[p]
	return e and e.v or nil
end

M.next_uid = 1000
M.next_iid = 5000
function M.new(view, fields, owner)
	local o = fields or {}
	o.__owner = owner or o.__owner or 'none'
	o.__cls = o.__cls or view
	M.next_iid = M.next_iid + 1
	o.instanceId = o.instanceId or M.next_iid
	return o, M.px(o, view)
end

-- cast(view, p, required_cls): a cast constructor (SoldierEntity(e) ...) checks the runtime class chain.
function M.cast(view, p)
	local o = M.raw(p)
	if o == nil then error('mock: ' .. view .. '(x) needs an engine object', 3) end
	local ok = false
	for _, t in ipairs(M.chain(o.__cls)) do if t == view then ok = true end end
	if not ok then error('mock: cannot cast ' .. tostring(o.__cls) .. ' to ' .. view, 3) end
	return M.px(o, view)
end

-- Is(name) for entities and data containers: the entity/data type name or any class of the chain (L32: the parent
-- data type matches when laws.is_parent is true, the default).
function M.is(o, name)
	if o.__etype == name or o.__dtype == name then return true end
	if o.__dparent == name and M.laws.is_parent ~= false then return true end
	for _, t in ipairs(M.chain(o.__cls)) do if t == name then return true end end
	return false
end

-- typeInfo: TypeInformation{name}
function M.typeinfo(name)
	local o = { name = name, __owner = 'none', __cls = 'TypeInformation' }
	return M.px(o, 'TypeInformation')
end

-- Events --------------------------------------------------------------------------------------------------------------
M.subs = {}
M.sub_seq = 0

local function handler_call(where, fn, ...)
	local ok, err = pcall(fn, ...)
	if not ok then
		M.errors[#M.errors + 1] = where .. ': ' .. tostring(err)
		M.print_raw('[error] [VeniceEXT] ' .. where .. ': ' .. tostring(err))
	end
	return ok, err
end
M.handler_call = handler_call

function M.dispatch(name, ...)
	local list = M.subs[name]
	if not list then return end
	local snapshot = {}
	for i, s in ipairs(list) do snapshot[i] = s end
	for _, s in ipairs(snapshot) do
		if not s.gone then
			if s.ctx ~= nil then handler_call(name, s.fn, s.ctx, ...) else handler_call(name, s.fn, ...) end
		end
	end
end

M.EVENT_OK = {}   -- name -> true for events valid in this realm (from the inventory, set by the host)

local function subscribe(name, a, b)
	if type(name) ~= 'string' then error('mock: Events:Subscribe needs a name', 3) end
	if not M.EVENT_OK[name] then error('mock: event ' .. name .. ' not in the inventory for ' .. M.realm, 3) end
	local fn, ctx = a, nil
	if b ~= nil then fn, ctx = b, a end
	if type(fn) ~= 'function' then error('mock: Events:Subscribe needs a function', 3) end
	M.sub_seq = M.sub_seq + 1
	local s = { fn = fn, ctx = ctx, name = name, __owner = 'none', __cls = 'Event' }
	M.subs[name] = M.subs[name] or {}
	table.insert(M.subs[name], s)
	return M.px(s, 'Event')
end

M.IMPL.Event = {
	Unsubscribe = function(s)
		s.gone = true
	end,
}

-- Hooks: priority order (higher first); HookContext:Call runs the original once and stops lower handlers.
M.hooks = {}
M.HOOK_OK = {}

function M.hook_install(name, prio, a, b)
	if not M.HOOK_OK[name] then error('mock: hook ' .. tostring(name) .. ' not in the inventory for ' .. M.realm, 3) end
	local fn, ctx = a, nil
	if b ~= nil then fn, ctx = b, a end
	local h = { fn = fn, ctx = ctx, prio = prio or 0, __owner = 'none', __cls = 'Hook' }
	M.hooks[name] = M.hooks[name] or {}
	table.insert(M.hooks[name], h)
	table.sort(M.hooks[name], function(x, y) return x.prio > y.prio end)
	return M.px(h, 'Hook')
end

M.IMPL.HookContext = {
	Call = function(hc)
		if hc.called then return hc.ret end
		hc.called = true
		hc.ret = hc.orig(table.unpack(hc.args, 1, hc.args.n))
		return hc.ret
	end,
}

-- run_hook(name, orig, ...): handlers in priority order; if none calls Call, the original runs (hooks.md).
function M.run_hook(name, orig, ...)
	local hc = { orig = orig, args = table.pack(...), called = false, __owner = 'none', __cls = 'HookContext' }
	local hp = M.px(hc, 'HookContext')
	for _, h in ipairs(M.hooks[name] or {}) do
		if h.ctx ~= nil then handler_call(name, h.fn, h.ctx, hp, ...) else handler_call(name, h.fn, hp, ...) end
		if hc.called then return hc.ret end
	end
	if not hc.called then
		hc.called = true
		hc.ret = orig(...)
	end
	return hc.ret
end

-- NetEvents: queued per realm; the host moves them to the other runtime between chunks.
M.net_subs = {}
M.net_out = {}

function M.net_subscribe(name, a, b)
	if type(name) ~= 'string' or name:sub(1, 3) ~= 'mh:' then error('mock: NetEvent name must be mh:*', 3) end
	local fn, ctx = a, nil
	if b ~= nil then fn, ctx = b, a end
	M.net_subs[name] = M.net_subs[name] or {}
	table.insert(M.net_subs[name], { fn = fn, ctx = ctx })
	return M.px({ __owner = 'none', __cls = 'NetEvent' }, 'NetEvent')
end

-- plain(v): deep copy of a NetEvent argument to plain values (tables of numbers/strings/bools only).
local function plain(v, depth)
	depth = depth or 0
	local t = type(v)
	if t == 'number' or t == 'string' or t == 'boolean' or t == 'nil' then return v end
	if t == 'table' and getmetatable(v) == nil and depth < 6 then
		local r = {}
		for k, x in pairs(v) do r[k] = plain(x, depth + 1) end
		return r
	end
	if M.is_vec3(v) then return { x = v.x, y = v.y, z = v.z, __vec3 = true } end
	error('mock: NetEvent argument of type ' .. t .. ' cannot be sent', 4)
end
M.plain = plain

function M.net_send(name, target, ...)
	if type(name) ~= 'string' or name:sub(1, 3) ~= 'mh:' then error('mock: NetEvent name must be mh:*', 3) end
	local args = table.pack(...)
	local pargs = { n = args.n }
	for i = 1, args.n do pargs[i] = plain(args[i]) end
	M.net_out[#M.net_out + 1] = { name = name, target = target, args = pargs }
end

function M.net_deliver(name, from, args)
	for _, s in ipairs(M.net_subs[name] or {}) do
		local a = {}
		for i = 1, args.n or #args do
			local x = args[i]
			if type(x) == 'table' and x.__vec3 then x = M.vec3(x.x, x.y, x.z) end
			a[i] = x
		end
		if from ~= nil then
			if s.ctx ~= nil then handler_call('net ' .. name, s.fn, s.ctx, from, table.unpack(a, 1, args.n or #args))
			else handler_call('net ' .. name, s.fn, from, table.unpack(a, 1, args.n or #args)) end
		else
			if s.ctx ~= nil then handler_call('net ' .. name, s.fn, s.ctx, table.unpack(a, 1, args.n or #args))
			else handler_call('net ' .. name, s.fn, table.unpack(a, 1, args.n or #args)) end
		end
	end
end

function M.take_net()
	local out = M.net_out
	M.net_out = {}
	return out
end

-- Enums ---------------------------------------------------------------------------------------------------------------
function M.enum_tables()
	local out = {}
	for name, vals in pairs(M.ENUMS) do
		local t = {}
		for k, v in pairs(vals) do t[k] = v end
		out[name] = setmetatable(t, {
			__index = function(_, k) error('mock: unknown enum value ' .. name .. '.' .. tostring(k), 2) end,
			__newindex = function() error('mock: enum ' .. name .. ' is read-only', 2) end,
		})
	end
	return out
end

-- json (VU adds json.encode/decode; content/vext/_index.md). The mock's own format, used only for the census sample.
local function jenc(v, depth)
	depth = depth or 0
	local t = type(v)
	if t == 'number' then return string.format('%.14g', v) end
	if t == 'string' then return '"' .. v:gsub('[%c"\\]', ' ') .. '"' end
	if t == 'boolean' then return tostring(v) end
	if t == 'nil' then return 'null' end
	if M.is_vec3(v) then return string.format('{"x":%.14g,"y":%.14g,"z":%.14g}', v.x, v.y, v.z) end
	if t == 'table' and depth < 8 then
		if v[1] ~= nil then
			local p = {}
			for i = 1, #v do p[i] = jenc(v[i], depth + 1) end
			return '[' .. table.concat(p, ',') .. ']'
		end
		local p = {}
		for k, x in pairs(v) do p[#p + 1] = '"' .. tostring(k) .. '":' .. jenc(x, depth + 1) end
		table.sort(p)
		return '{' .. table.concat(p, ',') .. '}'
	end
	return 'null'
end
M.json = { encode = function(v) return jenc(v) end, decode = function() return {} end }

-- Clock and output ------------------------------------------------------------------------------------------------------
function M.ms()
	return M.ms0 + math.floor(M.t * 1000 + 0.5)
end

function M.wall_seconds()
	return M.wall0 + M.t
end

function M.stamp()
	local s = M.wall_seconds()
	local day = math.floor(s / 86400)
	local sec = s - day * 86400
	return string.format('%02d:%02d:%06.3f', math.floor(sec / 3600), math.floor(sec / 60) % 60, sec % 60)
end

function M.print_raw(text)
	M.out[#M.out + 1] = { M.k, '[' .. M.stamp() .. '] ' .. text }
end

function M.mod_print(...)
	local n = select('#', ...)
	local parts = {}
	for i = 1, n do parts[i] = tostring((select(i, ...))) end
	M.out[#M.out + 1] = { M.k, '[' .. M.stamp() .. '] [info] [VeniceEXT] [mharness] ' .. table.concat(parts, '\t') }
end

function M.take_out()
	local out = M.out
	M.out = {}
	return out
end

-- os.date for the mod: the mock wall clock (2026-10-16 + wall0 + t), formats %Y %m %d %H %M %S only.
function M.os_date(fmt)
	fmt = fmt or '%c'
	local s = math.floor(M.wall_seconds())
	local day = 16 + s // 86400
	local sec = s % 86400
	local rep = { Y = '2026', m = '10', d = string.format('%02d', day), H = string.format('%02d', sec // 3600),
		M = string.format('%02d', (sec // 60) % 60), S = string.format('%02d', sec % 60) }
	return (fmt:gsub('%%(%a)', function(c) return rep[c] or ('%' .. c) end))
end

-- Mod sandbox (same rules as luaload.py: VU library whitelist, strict globals, VU require) ---------------------------
local function copy(t, drop)
	local r = {}
	for k, v in pairs(t) do if not (drop and drop[k]) then r[k] = v end end
	return r
end

-- build_env(sources, realm, globals) -> env, require
function M.build_env(sources, realm, globals)
	local env = {}
	local loaded = {}
	for _, n in ipairs({ 'assert', 'error', 'ipairs', 'next', 'pairs', 'pcall', 'rawequal', 'rawget', 'rawlen', 'rawset',
		'select', 'setmetatable', 'getmetatable', 'tonumber', 'tostring', 'type', 'xpcall' }) do env[n] = _G[n] end
	env.print = M.mod_print
	env._VERSION = _VERSION
	env.table = copy(table)
	env.string = copy(string, { dump = true })
	env.string.starts = function(a, b) return a:sub(1, #b) == b end
	env.string.ends = function(a, b) return b == '' or a:sub(-#b) == b end
	env.math = copy(math)
	env.os = copy(os, { execute = true, exit = true, getenv = true, remove = true, rename = true, setlocale = true,
		tmpname = true })
	env.os.date = M.os_date
	env.os.time = function() return math.floor(M.wall_seconds()) + 1792000000 end
	env.os.clock = function() return M.t end
	env.coroutine = copy(coroutine)
	env.utf8 = copy(utf8)
	env.package = { loaded = {} }
	env.json = M.json
	for k, v in pairs(globals) do env[k] = v end
	env._G = env
	local function resolve(path)
		if path:sub(-4) == '.lua' then path = path:sub(1, -5) end
		if path:find('%.') then return nil, 'require of a non-.lua path: ' .. path end
		if path:sub(1, 9) == '__shared/' then return 'shared/' .. path:sub(10) end
		return realm .. '/' .. path
	end
	env.require = function(path)
		if type(path) ~= 'string' then error('require needs a string', 2) end
		local key, why = resolve(path)
		if not key then error(why, 2) end
		if loaded[key] ~= nil then return loaded[key] end
		local src = sources[key]
		if not src then error('require: no file ext/' .. key .. '.lua', 2) end
		local chunk, err = load(src, '@ext/' .. key .. '.lua', 't', env)
		if not chunk then error(err, 0) end
		loaded[key] = true
		local r = chunk()
		if r ~= nil then loaded[key] = r end
		return loaded[key]
	end
	setmetatable(env, {
		__index = function(_, k)
			if k == 'MH' then return nil end
			error("undefined global '" .. tostring(k) .. "'", 2)
		end,
		__newindex = function(t, k, v)
			if k == 'MH' then rawset(t, k, v) else error("assignment to global '" .. tostring(k) .. "'", 2) end
		end,
	})
	return env, env.require
end

-- load_mod(sources, realm, order, vm): runs the two entries in the given order. vm 'separate' runs the shared entry in
-- its own env (its MH marker is then invisible to the realm entry).
function M.load_mod(sources, realm, order, vm, globals)
	M.mod_env = nil
	local env, req = M.build_env(sources, realm, globals)
	M.mod_env = env
	local errs = {}
	for _, entry in ipairs(order) do
		if entry == '__shared/__init__' and vm == 'separate' then
			local env2, req2 = M.build_env(sources, realm, globals)
			local ok, err = pcall(req2, entry)
			if not ok then errs[#errs + 1] = tostring(err) end
		else
			local ok, err = pcall(req, entry)
			if not ok then errs[#errs + 1] = tostring(err) end
		end
	end
	return errs
end

-- Line coverage (host side; the mod cannot see debug): counts executed lines per chunk name '@ext/...'.
M.cov = {}
function M.coverage_on()
	local cov = M.cov
	debug.sethook(function(_, line)
		local src = debug.getinfo(2, 'S').source
		if src:sub(1, 5) == '@ext/' then
			local c = cov[src]
			if c == nil then
				c = {}
				cov[src] = c
			end
			c[line] = true
		end
	end, 'l')
end
function M.coverage_off() debug.sethook() end

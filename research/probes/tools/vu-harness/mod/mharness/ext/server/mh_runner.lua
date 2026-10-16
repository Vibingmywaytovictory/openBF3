-- Scenario scheduler (DESIGN.md section 3.8). One active coroutine resumed per PostSim tick, a FIFO queue,
-- stop/pause/next, a timeout on every wait, the ticket budget and guard, level-change and no-human handling, cleanup
-- on every exit path. Admin actions (mapList.restartRound / mapList.runNextRound, RCON:SendCommand,
-- server/library/rcon.md) only as queue items the user typed or confirmed commands, each followed by fx act=admin
-- (the accepted deviation of DESIGN.md section 1).
local R = require('__shared/mh_row')
local C = require('__shared/mh_clock')
local chat = require('mh_chat')
local ids = require('mh_ids')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local meta = require('mh_meta')
local sites = require('mh_sites')
local REG = require('scenarios/registry')

local M = {}

M.GUARD = 60
M.WARN = 40
M.active = nil
M.items = {}           -- the queue: {kind='sc'|'rr'|'nextmap', sc, args, label, preset}
M.qpaused = false
M.qreason = nil
M.paused = false       -- !mh pause (active scenario holds at its next wait)
M.stop_req = false
M.last_done = {}       -- sc -> {i, n, label}
M.p0 = {}              -- Phase 0 results: tcost, level ('kept'|'lost'), damage ('YES'|'NO-arb'|'NO-aim'), gs ('srv'|'poll'), auto
M.tcost = { spawn = 1, kill = 1, remove = 1 }
M.initial = { [1] = nil, [2] = nil }
M.level = nil
M.level_k = 0
M.level_epoch = 0
M.humans_present = true
M.warned = {}
M.done_items = 0
M.history = {}
M.cert = nil

local function load_cert()
	local ok, c = pcall(require, '__shared/mh_cert')
	if ok and type(c) == 'table' then M.cert = c else M.cert = { gate = 'none', ids = {} } end
end
load_cert()

-- certified(key): key 'P0', 'WA', 'WB:U', 'WB:B', 'SA:U', ...; P0 is always allowed.
function M.certified(key)
	if key == 'P0' then return true end
	local set = M.cert.ids or {}
	if set[key] then return true end
	local base = key:match('^(%u%w)')
	return base ~= nil and set[base] == true
end

-- cert_key(sc, args): WB and SA are certified per mode (WB:U, WB:B, SA:U, SA:B); others by id.
function M.cert_key(sc, args)
	if sc == 'WB' or sc == 'SA' then return sc .. ':' .. ((args and args.mode) or 'U') end
	return sc
end

local function err_obj(kind, why) return { mh = kind, why = why } end

-- Context ----------------------------------------------------------------------------------------------------------
local Ctx = {}
Ctx.__index = Ctx

function Ctx:guard()
	if M.stop_req then error(err_obj('stop', 'stop'), 0) end
	if self.trip then error(err_obj('guard', self.trip), 0) end
	if self.level_lost then error(err_obj('level', 'level destroyed'), 0) end
	if self.skip_req and self.in_cfg then
		self.skip_req = false
		error(err_obj('skip', 'next'), 0)
	end
end

local function yield_tick(ctx)
	ctx.act_k = C.gk
	coroutine.yield()
	ctx:guard()
end

local function held()
	return M.paused or (M.active and M.active.hold_reason ~= nil)
end

function Ctx:wait_ticks(n)
	local target = C.gk + math.max(1, n)
	while C.gk < target do
		yield_tick(self)
		if held() then target = target + 1 end
	end
end

function Ctx:wait(s)
	self:wait_ticks(math.max(1, math.floor(s * C.hzv + 0.5)))
end

function Ctx:wait_until(pred, timeout_s, label)
	local deadline = C.gk + math.max(1, math.floor(timeout_s * C.hzv + 0.5))
	self.wait_label = label
	self.wait_deadline = deadline
	while true do
		local ok, r = pcall(pred)
		if ok and r then
			self.wait_label = nil
			return true
		end
		if C.gk >= deadline then
			self:mark('timeout:' .. tostring(label))
			self.wait_label = nil
			return false
		end
		yield_tick(self)
		if held() then deadline = deadline + 1 end
	end
end

function Ctx:mark(lbl, f)
	f = f or {}
	R.row('mark', { lbl = R.label(lbl), cfg = f.cfg, i = f.i, n = f.n, x = f.x, k0 = f.k0, k1 = f.k1, own = f.own })
end

function Ctx:say(text)
	chat.say(self.owner, text)
end

function Ctx:prompt(kind, fields, text)
	chat.prompt(self.owner, kind, fields, text)
end

function Ctx:progress(done, total, cfg)
	self.done, self.total = done, total
	local t1, t2 = S.tickets(1), S.tickets(2)
	local fc1, fc2 = M.forecast_active()
	local eta = nil
	if done > 0 and total > done then
		eta = (C.gk - self.k_start) / C.hzv * (total - done) / done
	end
	R.row('prog', { done = done, total = total, cfg = cfg, eta = eta, tk1 = t1, tk2 = t2, fc1 = fc1, fc2 = fc2 })
	chat.progress(self.owner, 'prog.' .. self.sc, string.format('%s %d/%d%s', self.sc, done, total,
		eta and string.format(', %d min left', math.floor(eta / 60 + 0.5)) or ''), C.gt)
end

function Ctx:need_site(name)
	local s = sites.get(name)
	if s == nil then
		self:mark('no-site:' .. name)
		self:say(self.sc .. ' needs site ' .. name .. ' (!mh ' .. name .. ')')
	end
	return s
end

-- abort(why): ends the run; the runner writes the one 'aborted' end mark with the reason.
function Ctx:abort(why)
	error(err_obj('abort', why), 0)
end

-- bot(team, w, opt): a bot owned by this run (removed at cleanup).
function Ctx:bot(team, w, opt)
	local b = bots.create(team, w, opt)
	if b then self.bots[#self.bots + 1] = b end
	return b
end

function Ctx:drop_bot(b)
	for i, x in ipairs(self.bots) do
		if x == b then
			table.remove(self.bots, i)
			break
		end
	end
	S.unwatch(b.sid)
	bots.remove(b)
end

function Ctx:check_tickets()
	for team = 1, 2 do
		local t = S.tickets(team)
		if t and t < M.GUARD then
			self.trip = string.format('T%d %d < %d', team, math.floor(t), M.GUARD)
			self:guard()
		end
	end
end

-- config(i, n, label, x, fn): cfg-start / cfg-done / cfg-skip; three consecutive skips abort the scenario.
function Ctx:config(i, n, label, x, fn)
	self.cfg_label = label
	self.cfg_i = i
	self:mark('cfg-start', { cfg = label, i = i, n = n, x = x })
	self:check_tickets()
	self.in_cfg = true
	local ok, res = pcall(fn)
	self.in_cfg = false
	if ok and res ~= false then
		self:mark('cfg-done', { cfg = label, i = i, n = n })
		M.last_done[self.sc] = { i = i, n = n, label = label }
		self.skips = 0
		self:say(string.format('%s cfg %d/%d done', self.sc, i, n))
		self:progress(i, n, label)
		return true
	end
	if not ok and type(res) == 'table' and res.mh ~= 'skip' then error(res, 0) end
	local why = ok and 'setup' or (type(res) == 'table' and res.why or R.label(tostring(res)))
	if not ok and type(res) ~= 'table' then
		R.row('err', { where = 'cfg.' .. self.sc, msg = R.label(tostring(res)):sub(1, 120), n = 1 })
	end
	self:mark('cfg-skip', { cfg = label, i = i, n = n, x = { why = R.label(why) } })
	self.skips = self.skips + 1
	if self.cleanup_cfg then pcall(self.cleanup_cfg, self) end
	if self.skips >= 3 then self:abort('3 consecutive skips') end
	return false
end

-- Scenario modules ---------------------------------------------------------------------------------------------------
function M.module(sc)
	local e = REG[sc]
	if e == nil or not e.enabled then return nil end
	return e.mod
end

-- Ticket forecast (DESIGN 3.8, 6.12) --------------------------------------------------------------------------------
local function cost_tickets(c)
	local tc = M.tcost
	local t1 = (c.s1 or 0) * tc.spawn + (c.d1 or 0) * tc.kill + (c.r1 or 0) * tc.remove + (c.b1 or 0)
	local t2 = (c.s2 or 0) * tc.spawn + (c.d2 or 0) * tc.kill + (c.r2 or 0) * tc.remove + (c.b2 or 0)
	return t1, t2
end
M.cost_tickets = cost_tickets

function M.initial_for(level)
	local a, b = S.initial_tickets(1), S.initial_tickets(2)
	if level == M.level and a and b then return a, b end
	if level and level:find('MP_001') then return 300, 300 end -- Bazaar CQL [inferred]
	return M.initial[1] or 350, M.initial[2] or 350
end

function M.forecast_active()
	local a = M.active
	if a == nil or a.cost == nil then return nil, nil end
	local t1, t2 = cost_tickets(a.cost)
	local c1, c2 = S.tickets(1), S.tickets(2)
	if c1 == nil or c2 == nil then return nil, nil end
	local frac = 0
	if a.ctx and a.ctx.total and a.ctx.total > 0 then frac = (a.ctx.done or 0) / a.ctx.total end
	return c1 - t1 * (1 - frac), c2 - t2 * (1 - frac)
end

local PRESETS = {
	['WA:tier1'] = { sc = 'WA', args = { tier = '1' } },
	['WA:rest'] = { sc = 'WA', args = { tier = 'rest' } },
	['WA:all'] = { sc = 'WA', args = { tier = 'all' } },
	['WA:60'] = { sc = 'WA', args = { tier = '60' } },
	['WB:B'] = { sc = 'WB', args = { mode = 'B' } },
	['SA:B'] = { sc = 'SA', args = { mode = 'B' } },
	SB = { sc = 'SB', args = {} },
	MB = { sc = 'MB', args = {} },
	MA = { sc = 'MA', args = {} },
}

-- parse_item('WA:all:from=8') -> item or nil, reason
function M.parse_item(tok)
	if tok == 'rr' or tok == 'nextmap' then return { kind = tok, label = tok } end
	local base, rest = tok:match('^([%w]+:[%w]+)(.*)$')
	if not PRESETS[base or ''] then base, rest = tok:match('^([%w]+)(.*)$') end
	local p = PRESETS[base or '']
	if not p then return nil, 'unknown item ' .. tok end
	local args = {}
	for key, v in pairs(p.args) do args[key] = v end
	for kv in (rest or ''):gmatch(':([^:]+)') do
		local key, v = kv:match('^(%w+)=([%w.,]+)$')
		if not key then return nil, 'bad argument ' .. kv end
		args[key] = v
	end
	return { kind = 'sc', sc = p.sc, args = args, label = tok, preset = base }
end

-- validate(items): simulate the queue: sites, level, P0 results, certification, ticket forecast. Returns lines or
-- nil, reason (the whole line is refused).
function M.validate(items)
	local state = { level = M.level, sites_ok = true, t1 = S.tickets(1) or 350, t2 = S.tickets(2) or 350, eta = 0 }
	local lines = {}
	for idx, it in ipairs(items) do
		if it.kind == 'rr' or it.kind == 'nextmap' then
			if M.p0.level ~= 'kept' then
				return nil, it.label .. ' refused: round restart not verified in this Lua state (!mh p0 level confirm)'
			end
			if not M.certified(it.kind) then return nil, it.label .. ' not certified tonight' end
			if it.kind == 'nextmap' then
				state.level = 'Levels/MP_001/MP_001'
				state.sites_ok = false
			end
			state.t1, state.t2 = M.initial_for(state.level)
			state.eta = state.eta + 150
		else
			local mod = M.module(it.sc)
			if mod == nil then return nil, it.label .. ' refused: unknown scenario' end
			if not M.certified(M.cert_key(it.sc, it.args)) then return nil, it.label .. ' not certified tonight' end
			local ok, why = M.requirements(it.sc, mod, it.args, state)
			if not ok then return nil, it.label .. ' refused: ' .. why end
			local c = mod.cost(it.args, M)
			local d1, d2 = cost_tickets(c)
			state.t1, state.t2 = state.t1 - d1, state.t2 - d2
			if state.t1 < M.GUARD or state.t2 < M.GUARD then
				return nil, string.format('%s refused: forecast T1 %d T2 %d < %d, put rr before it', it.label,
					math.floor(state.t1), math.floor(state.t2), M.GUARD)
			end
			state.eta = state.eta + mod.estimate(it.args)
		end
		lines[#lines + 1] = string.format('%d %s +%dmin T1 %d T2 %d', idx, it.label, math.floor(state.eta / 60 + 0.5),
			math.floor(state.t1), math.floor(state.t2))
	end
	return lines
end

-- Every missing prerequisite at once, in the order the user fixes them (aim before damage: P0.9 needs P0.5), so one
-- refusal names the whole fix after a new Lua state (server restart, crash, lost state; RUNBOOK "re-prepare").
function M.requirements(sc, mod, args, state)
	local need = mod.needs and mod.needs(args) or {}
	state = state or { level = M.level, sites_ok = true }
	local miss = {}
	if need.levels then
		local okl = false
		for _, l in ipairs(need.levels) do
			if state.level and state.level:find(l, 1, true) then okl = true end
		end
		if not okl then miss[#miss + 1] = 'needs level ' .. table.concat(need.levels, '/') end
	end
	for _, s in ipairs(need.sites or {}) do
		if sites.get(s) == nil then miss[#miss + 1] = 'site ' .. s .. ' missing (!mh ' .. s .. ')'
		elseif not state.sites_ok then miss[#miss + 1] = 'site ' .. s .. ' is on another level' end
	end
	if need.aimcal and bots.cal == nil then miss[#miss + 1] = 'aim calibration missing (!mh p0 aim)' end
	if need.botdmg and M.p0.damage ~= 'YES' then
		if M.p0.damage == 'NO-arb' then
			-- P0.9 already said NO (arbitration): repeating it does not help, the user shoots instead (RUNBOOK failures)
			miss[#miss + 1] = 'bot damage is NO (arbitration): use mode=U (you shoot)'
		else
			miss[#miss + 1] = 'bot damage is not YES (P0.9: !mh p0 damage' .. (bots.cal == nil and ' after !mh p0 aim)' or ')')
		end
	end
	if #miss > 0 then return false, table.concat(miss, '; ') end
	return true
end

-- Start / finish -----------------------------------------------------------------------------------------------------
function M.start(sc, args, owner, item)
	if M.active then return false, 'already running ' .. M.active.sc end
	local mod
	if sc == 'ADMIN' then mod = M.admin else mod = M.module(sc) end
	if mod == nil then return false, 'unknown scenario ' .. tostring(sc) end
	args = args or {}
	local run = C.start_run(sc == 'ADMIN' and 'S' or sc)
	local ctx = setmetatable({ sc = sc, args = args, owner = owner, skips = 0, bots = {}, k_start = C.gk, act_k = C.gk,
		run = run, item = item }, Ctx)
	meta.run_meta(sc, args, M)
	local cost = (sc ~= 'ADMIN' and mod.cost) and mod.cost(args, M) or nil
	local co = coroutine.create(function() return mod.run(ctx, args) end)
	M.active = { sc = sc, mod = mod, ctx = ctx, co = co, run = run, cost = cost, item = item, started = C.gk }
	M.stop_req = false
	M.paused = false
	ctx:mark('run-start', { x = { sc = sc } })
	if sc ~= 'ADMIN' then
		local est = mod.estimate and mod.estimate(args) or 0
		chat.say(owner, string.format('%s started (run %s), about %d min', sc, run:sub(-6), math.floor(est / 60 + 0.5)))
	end
	return true
end

local function finish(status, why)
	local a = M.active
	if a == nil then return end
	local ctx = a.ctx
	pcall(function() if a.mod.cleanup then a.mod.cleanup(ctx) end end)
	for _, b in ipairs(ctx.bots) do
		pcall(function()
			S.unwatch(b.sid)
			bots.release_all(b)
			bots.remove(b)
		end)
	end
	S.tk_mode = '1hz'
	S.cp_mode = 'change'
	S.cp_active = {}
	H.human_watch = {}
	R.row('mark', { lbl = R.label(status), x = { why = why and R.label(why) or nil } })
	pcall(meta.client_ctl, ctx.owner, nil)
	C.end_run()
	M.active = nil
	M.history[#M.history + 1] = { sc = a.sc, status = status }
	if a.sc ~= 'ADMIN' or status ~= 'done' then
		chat.say(ctx.owner, string.format('%s %s%s', a.sc == 'ADMIN' and (a.item and a.item.label or 'admin') or a.sc,
			status, why and (': ' .. why) or ''))
	end
	if status == 'stopped' then
		M.qpaused = true
		M.qreason = 'stopped'
	end
	M.stop_req = false
end
M.finish = finish

-- Guard trip: requeue the item from its next config at the front and pause the queue.
local function guard_trip(why)
	local a = M.active
	local item = a.item
	local sc = a.sc
	finish('guard', why)
	if sc ~= 'ADMIN' then
		local nxt = { kind = 'sc', sc = sc, args = {}, label = sc }
		if item then
			for key, v in pairs(item.args or {}) do nxt.args[key] = v end
			nxt.label = item.label
			nxt.preset = item.preset
		else
			for key, v in pairs(a.ctx.args or {}) do nxt.args[key] = v end
		end
		local ld = M.last_done[sc]
		if ld then nxt.args.from = tostring(ld.i + 1) end
		table.insert(M.items, 1, nxt)
	end
	M.qpaused = true
	M.qreason = 'tickets'
	chat.say(nil, 'tickets low (' .. why .. '), queue paused: !mh restartround confirm, then !mh queue resume')
end

function M.step()
	local a = M.active
	if a then
		if M.stop_req then
			finish('stopped')
			return
		end
		if a.hold_reason == nil and not M.paused and C.gk - a.ctx.act_k > 120 * C.hzv then
			finish('stalled', 'no progress for 120 s')
			return
		end
		-- ticket guard every 10 s
		if a.sc ~= 'ADMIN' and (C.gk - a.started) % (10 * C.hzv) == 0 and not a.ctx.trip then
			for team = 1, 2 do
				local t = S.tickets(team)
				if t and t < M.GUARD and not a.mod.own_guard then
					guard_trip(string.format('T%d %d', team, math.floor(t)))
					return
				end
				if t and t < M.GUARD + M.WARN and not M.warned[team] then
					M.warned[team] = true
					chat.say(a.ctx.owner, string.format('warning: team %d tickets %d', team, math.floor(t)))
				end
			end
		end
		local ok, err = coroutine.resume(a.co)
		if coroutine.status(a.co) == 'dead' then
			if ok then
				finish('done')
			elseif type(err) == 'table' and err.mh then
				if err.mh == 'guard' then
					guard_trip(err.why)
				elseif err.mh == 'stop' then
					finish('stopped')
				elseif err.mh == 'level' then
					finish('aborted', 'level destroyed')
				else
					finish('aborted', err.why)
				end
			else
				R.row('err', { where = 'run.' .. a.sc, msg = R.label(tostring(err)):sub(1, 120), n = 1 })
				finish('aborted', 'error')
			end
			M.done_items = M.done_items + 1
		end
		return
	end
	-- idle: next queue item
	if #M.items > 0 and not M.qpaused and M.humans_present and C.gk - M.level_k > 15 * C.hzv then
		local it = table.remove(M.items, 1)
		local owner = ids.human_list()[1]
		if it.kind == 'rr' or it.kind == 'nextmap' then
			M.start('ADMIN', { cmd = it.kind }, owner, it)
		else
			local mod = M.module(it.sc)
			local ok, why = M.requirements(it.sc, mod, it.args)
			if not ok then
				chat.say(owner, it.label .. ' skipped: ' .. why)
				R.row('mark', { lbl = 'queue-skip', x = { item = R.label(it.label), why = R.label(why) } })
			else
				M.start(it.sc, it.args, owner, it)
			end
		end
	end
end

-- Admin items (DESIGN 1, 3.8): restart round / next map ---------------------------------------------------------------
M.admin = { id = 'ADMIN' }

function M.send_admin(cmd, sid)
	local res = nil
	if cmd == 'rr' then
		res = RCON:SendCommand('mapList.restartRound')
	else
		res = RCON:SendCommand('mapList.runNextRound')
	end
	local ok = type(res) == 'table' and res[1] == 'OK'
	bots.fx('admin', sid or 0, ok, { arg = { cmd = cmd } })
	return ok
end

function M.admin.run(ctx, args)
	local epoch = M.level_epoch
	ctx.expect_level = true
	if not M.send_admin(args.cmd) then ctx:abort('RCON refused ' .. args.cmd) end
	if not ctx:wait_until(function() return M.level_epoch > epoch and C.gk - M.level_k > 15 * C.hzv end, 240,
			'level') then
		ctx:abort('level did not reload')
	end
	M.warned = {}
end

function M.admin.cleanup(ctx) end

-- Commands -----------------------------------------------------------------------------------------------------------
function M.stop()
	if not M.active then return false end
	M.stop_req = true
	return true
end

function M.enqueue(items)
	for _, it in ipairs(items) do M.items[#M.items + 1] = it end
	M.qpaused = false
	M.qreason = nil
end

function M.resume_queue()
	M.qpaused = false
	M.qreason = nil
	M.paused = false
	M.warned = {}
end

function M.clear_queue()
	M.items = {}
end

function M.skip()
	if M.active and M.active.ctx.in_cfg then
		M.active.ctx.skip_req = true
		return true
	end
	return false
end

-- Level and humans ---------------------------------------------------------------------------------------------------
function M.on_level_destroy()
	R.row('ev', { name = 'Level:Destroy' })
	bots.on_level_destroy()
	local a = M.active
	if a and not a.ctx.expect_level then a.ctx.level_lost = true end
	H.tracked = {}
end

function M.on_level_loaded(levelName, gameMode, round, roundsPerMap)
	if M.level ~= nil and M.level ~= levelName then sites.invalidate() end
	M.level = levelName
	M.level_k = C.gk
	M.level_epoch = M.level_epoch + 1
	S.on_level()
	ids.reset_level()
	local a, b = S.initial_tickets(1), S.initial_tickets(2)
	if a then M.initial[1] = a end
	if b then M.initial[2] = b end
end

function M.on_human_left()
	if #ids.human_list() == 0 then
		M.humans_present = false
		if M.active then M.active.hold_reason = 'human-left' end
		R.row('mark', { lbl = 'human-left' })
	end
end

function M.on_human_seen()
	if not M.humans_present then
		M.humans_present = true
		if M.active then M.active.hold_reason = nil end
		R.row('mark', { lbl = 'human-back' })
	end
end

function M.status_lines()
	local out = {}
	local a = M.active
	if a then
		local c = a.ctx
		out[#out + 1] = string.format('%s run %s cfg %s %s/%s%s%s', a.sc, a.run:sub(-6), tostring(c.cfg_label or '-'),
			tostring(c.done or 0), tostring(c.total or '?'), M.paused and ' PAUSED' or '',
			a.hold_reason and (' HOLD ' .. a.hold_reason) or '')
		if c.wait_label then
			out[#out + 1] = string.format('waiting %s, timeout in %ds', c.wait_label,
				math.max(0, math.floor((c.wait_deadline - C.gk) / C.hzv)))
		end
	else
		out[#out + 1] = 'idle'
	end
	local q = {}
	for i, it in ipairs(M.items) do q[#q + 1] = i .. ':' .. it.label end
	out[#out + 1] = 'queue ' .. (#q > 0 and table.concat(q, ' ') or 'empty') .. (M.qpaused and (' (paused ' ..
		tostring(M.qreason) .. ')') or '')
	local t1, t2 = S.tickets(1), S.tickets(2)
	out[#out + 1] = string.format('bots %d, tickets T1 %s T2 %s, rows %d, drops %d, errors %d', bots.count(),
		t1 and math.floor(t1) or '?', t2 and math.floor(t2) or '?', R.stats.rows, R.stats.dropped, R.stats.errs)
	local ld = {}
	for sc, d in pairs(M.last_done) do ld[#ld + 1] = string.format('%s %d/%d', sc, d.i, d.n) end
	table.sort(ld)
	if #ld > 0 then out[#out + 1] = 'last cfg-done: ' .. table.concat(ld, ', ') end
	return out
end

return M

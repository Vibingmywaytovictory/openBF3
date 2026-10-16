-- '!mh' command parser and table (DESIGN.md sections 3.7, 5).
-- Player:Chat(player, recipientMask, message) (server/event/player_chat.md); the client console command 'mh' arrives
-- as NetEvent 'mh:cmd' (client/library/console.md, server/library/netevents.md) and is handled the same way.
-- Tokens [A-Za-z0-9_.,:=+-]{1,32}, at most 12; every command emits a 'cmd' row; every verb is safe to repeat.
-- Admin verbs (nextmap, restartround) need 'confirm' and run through mh_runner's ADMIN item (fx act=admin).
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local chat = require('mh_chat')
local ids = require('mh_ids')
local bots = require('mh_bots')
local S = require('mh_samplers')
local H = require('mh_hooks')
local meta = require('mh_meta')
local sites = require('mh_sites')
local RUN = require('mh_runner')
local W = require('scenarios/wa_cfg')

local M = {}

M.verbs = {
	'help', 'status', 'census', 'p0', 'wall', 'range', 'flat', 'ring', 'sites', 'run', 'queue', 'stop', 'pause', 'resume',
	'next', 'mark', 'ok', 'target', 'untarget', 'clear', 'watch', 'unwatch', 'rate', 'nextmap', 'restartround',
}

M.HELP = {
	help = 'help [verb]',
	status = 'status: active scenario, queue, tickets, rows',
	census = 'census: API, entity types, assets (P0.2)',
	p0 = 'p0 round|level confirm|tick|aim|bots|fire|damage|human|log|report|all',
	wall = 'wall: look at a flat wall 10-25 m away', range = 'range: look along a clear lane',
	flat = 'flat: look along flat open ground', ring = 'ring: look at open ground (M67 ring centre)',
	sites = 'sites: list recorded sites',
	run = 'run <SC> [k=v ...]  SC: P0 WA SB WB SA MA MB WC WD',
	queue = 'queue <item> ... | queue | queue clear | queue resume  items: WA:tier1 WA:rest WA:all WA:60 SB MB MA WB:B ' ..
		'SA:B rr nextmap',
	stop = 'stop: stop the active scenario and pause the queue', pause = 'pause / resume the active scenario',
	resume = 'resume the active scenario', next = 'next: skip the current config', mark = 'mark <label>',
	ok = 'ok: confirm a prompt', target = 'target <d>|here', untarget = 'untarget', clear = 'clear: remove every bot',
	watch = 'watch me', unwatch = 'unwatch me', rate = 'rate <gsEvery>', nextmap = 'nextmap confirm',
	restartround = 'restartround confirm',
}

M.MAX_TOKENS = 12
M.manual = nil  -- manual target bot (target/untarget)

-- tokens(text) -> list | nil, reason
function M.tokens(text)
	if type(text) ~= 'string' then return nil, 'no text' end
	local out = {}
	for tok in text:gmatch('%S+') do
		if #tok > 32 then return nil, 'token too long' end
		if tok:find('[^A-Za-z0-9_%.,:=%+%-]') then return nil, 'bad character in ' .. R.label(tok):sub(1, 32) end
		out[#out + 1] = tok
		if #out > M.MAX_TOKENS then return nil, 'too many tokens' end
	end
	return out
end

local function kv_args(toks, first)
	local args = {}
	for i = first, #toks do
		local key, v = toks[i]:match('^(%w+)=([%w.,:+-]+)$')
		if key then args[key] = v else args[toks[i]] = toks[i] end
	end
	return args
end

local function reply(pl, text) chat.say(pl, text) end

local function reply_lines(pl, lines)
	for i = 1, math.min(#lines, chat.MAX_MSGS) do reply(pl, lines[i]) end
end

-- Handlers ---------------------------------------------------------------------------------------------------------
local H_ = {}

function H_.help(pl, toks)
	local v = toks[2]
	if v and M.HELP[v] then
		reply(pl, M.HELP[v])
	else
		reply(pl, 'verbs: ' .. table.concat(M.verbs, ' '))
	end
	return true
end

function H_.status(pl)
	reply_lines(pl, RUN.status_lines())
	return true
end

function H_.census(pl)
	local s = meta.census(pl)
	reply(pl, string.format('census: API %d/%d, assets %d/%d, entity types %d, flags %d, ticket counters %d, vm %s, ' ..
		'console %s', s.api, s.apin, s.res, s.resn, s.types, s.flags, s.tcs, s.vm, s.cmd))
	if #s.missing > 0 then reply(pl, 'missing assets: ' .. table.concat(s.missing, ' ')) end
	return true
end

function H_.p0(pl, toks)
	local step = toks[2] or 'report'
	local args = kv_args(toks, 3)
	args.step = step
	if toks[3] == 'confirm' then args.confirm = 'confirm' end
	local ok, why = RUN.start('P0', args, pl)
	if not ok then reply(pl, why) end
	return ok
end

local function site_cmd(pl, name, args)
	sites.request(name, pl, args, function(s, why)
		reply(pl, sites.describe(name, s, why))
	end)
	return true
end
function H_.wall(pl) return site_cmd(pl, 'wall') end
function H_.range(pl) return site_cmd(pl, 'range') end
function H_.flat(pl) return site_cmd(pl, 'flat') end
function H_.ring(pl, toks) return site_cmd(pl, 'ring', { radii = toks[2] }) end

function H_.sites(pl)
	local l = sites.list()
	reply(pl, #l > 0 and table.concat(l, '; ') or 'no sites recorded')
	return true
end

local SC = { P0 = true, WA = true, SB = true, WB = true, SA = true, MA = true, MB = true, WC = true, WD = true }

local function check_weapons(args)
	if args.w == nil then return true end
	for tok in args.w:gmatch('[^,]+') do
		if W.canonical(tok) == nil then
			local names = {}
			for key in pairs(W.unlocks) do names[#names + 1] = key end
			table.sort(names)
			return false, 'unknown weapon ' .. tok .. ' (valid: ' .. table.concat(names, ' ') .. ')'
		end
	end
	return true
end

function H_.run(pl, toks)
	local sc = toks[2] and toks[2]:upper() or nil
	if sc == nil or not SC[sc] then
		reply(pl, 'run <SC>: P0 WA SB WB SA MA MB WC WD')
		return false
	end
	if RUN.active then
		reply(pl, 'already running ' .. RUN.active.sc)
		return false
	end
	local args = kv_args(toks, 3)
	local okw, whyw = check_weapons(args)
	if not okw then
		reply(pl, whyw)
		return false
	end
	if sc ~= 'P0' then
		local lines, why = RUN.validate({ { kind = 'sc', sc = sc, args = args, label = sc } })
		if lines == nil then
			reply(pl, why)
			return false
		end
	end
	local ok, why = RUN.start(sc, args, pl, { kind = 'sc', sc = sc, args = args, label = sc })
	if not ok then reply(pl, why) end
	return ok
end

function H_.queue(pl, toks)
	local sub = toks[2]
	if sub == nil then
		local lines = RUN.validate(RUN.items) or {}
		if #lines == 0 then lines = { 'queue empty' } end
		reply_lines(pl, lines)
		return true
	end
	if sub == 'clear' then
		RUN.clear_queue()
		reply(pl, 'queue cleared')
		return true
	end
	if sub == 'resume' then
		RUN.resume_queue()
		reply(pl, 'queue resumed (' .. #RUN.items .. ' items)')
		return true
	end
	local items = {}
	for i = 2, #toks do
		local it, why = RUN.parse_item(toks[i])
		if it == nil then
			reply(pl, 'nothing queued: ' .. why)
			return false
		end
		local okw, whyw = check_weapons(it.args or {})
		if not okw then
			reply(pl, 'nothing queued: ' .. whyw)
			return false
		end
		items[#items + 1] = it
	end
	-- validate the existing queue plus the new items as one plan
	local all = {}
	for _, it in ipairs(RUN.items) do all[#all + 1] = it end
	for _, it in ipairs(items) do all[#all + 1] = it end
	local lines, why = RUN.validate(all)
	if lines == nil then
		reply(pl, 'nothing queued: ' .. why)
		return false
	end
	RUN.enqueue(items)
	reply_lines(pl, lines)
	return true
end

function H_.stop(pl)
	if RUN.stop() then
		reply(pl, 'stopping ' .. RUN.active.sc .. ' (queue paused)')
		return true
	end
	RUN.qpaused = #RUN.items > 0 or RUN.qpaused
	reply(pl, 'nothing running')
	return true
end

function H_.pause(pl)
	RUN.paused = true
	reply(pl, RUN.active and ('paused ' .. RUN.active.sc) or 'nothing running (paused flag set)')
	return true
end

function H_.resume(pl)
	RUN.paused = false
	reply(pl, RUN.active and ('resumed ' .. RUN.active.sc) or 'nothing running')
	return true
end

function H_.next(pl)
	if RUN.skip() then
		reply(pl, 'skipping the current config')
	else
		reply(pl, 'nothing to skip')
	end
	return true
end

function H_.mark(pl, toks)
	local lbl = {}
	for i = 2, #toks do lbl[#lbl + 1] = toks[i] end
	R.row('mark', { lbl = R.label('user:' .. table.concat(lbl, ' ')), x = { who = ids.player(pl) } })
	reply(pl, 'marked')
	return true
end

function H_.ok(pl)
	local a = RUN.active
	if a then
		a.ctx.ok_flag = true
		return true
	end
	return true
end

-- target <d>|here: a manual target bot (team opposite the caller) d m along the caller's view, or at the caller.
local function caller_pose(pl)
	local p, f = nil, nil
	pcall(function()
		p = V.v(pl.soldier.worldTransform.trans)
		f = V.horiz(V.v(pl.soldier.worldTransform.forward))
	end)
	local cam = sites.cams[ids.player(pl)]
	if cam then f = V.horiz(cam.fwd) end
	return p, f
end

function H_.untarget(pl)
	if M.manual then
		S.unwatch(M.manual.sid)
		bots.remove(M.manual)
		M.manual = nil
		reply(pl, 'target removed')
	else
		reply(pl, 'no target')
	end
	return true
end

function H_.target(pl, toks)
	local p, f = caller_pose(pl)
	if p == nil then
		reply(pl, 'spawn first')
		return false
	end
	local d = toks[2] == 'here' and 0 or (tonumber(toks[2] or '10') or 10)
	local team = 2
	pcall(function() if math.tointeger(pl.teamId) == 2 then team = 1 end end)
	if M.manual then H_.untarget(pl) end
	local pos = V.add(p, V.scale(f or { 0, 0, 1 }, d))
	pos = sites.ground(pos, 3, 8) or pos
	local b = bots.create(team, team == 1 and 'M16A3' or 'AK74M')
	if b == nil then
		reply(pl, 'target: create failed')
		return false
	end
	if not bots.spawn(b, pos, V.scale(f or { 0, 0, 1 }, -1), 0) then
		bots.remove(b)
		reply(pl, 'target: spawn failed')
		return false
	end
	M.manual = b
	S.watch(b.sid, { hp = true })
	reply(pl, string.format('target %d at %.0f m', b.sid, d))
	return true
end

function H_.clear(pl)
	if RUN.active then
		reply(pl, 'running ' .. RUN.active.sc .. ': !mh stop first')
		return false
	end
	M.manual = nil
	S.unwatch_all()
	local n = bots.remove_all()
	reply(pl, 'removed ' .. n .. ' bots')
	return true
end

function H_.watch(pl)
	local sid = ids.player(pl)
	S.watch(sid, { hp = true, gs = true, wf = true })
	H.human_watch[sid] = true
	reply(pl, 'watching you (id ' .. sid .. ')')
	return true
end

function H_.unwatch(pl)
	local sid = ids.player(pl)
	S.unwatch(sid)
	H.human_watch[sid] = nil
	reply(pl, 'unwatched')
	return true
end

function H_.rate(pl, toks)
	local n = math.tointeger(tonumber(toks[2] or '1'))
	if n == nil or n < 1 or n > 30 then
		reply(pl, 'rate <1..30>')
		return false
	end
	S.gs_every = n
	reply(pl, 'gs every ' .. n .. ' ticks')
	return true
end

local function admin(pl, toks, kind, label)
	if toks[2] ~= 'confirm' then
		reply(pl, label .. ' changes the round: type !mh ' .. label .. ' confirm')
		return false
	end
	if RUN.active then
		reply(pl, 'already running ' .. RUN.active.sc)
		return false
	end
	local ok, why = RUN.start('ADMIN', { cmd = kind }, pl, { kind = kind, label = label })
	if not ok then reply(pl, why) end
	return ok
end
function H_.nextmap(pl, toks) return admin(pl, toks, 'nextmap', 'nextmap') end
function H_.restartround(pl, toks) return admin(pl, toks, 'rr', 'restartround') end

M.handlers = H_

-- handle(player, text, via): text without the '!mh' prefix.
function M.handle(pl, text, via)
	local who = ids.player(pl) or 0
	RUN.on_human_seen()
	local toks, why = M.tokens(text)
	if toks == nil then
		R.row('cmd', { who = who, verb = 'invalid', ok = false, args = R.label(why) })
		reply(pl, 'invalid command: ' .. why)
		return false
	end
	local verb = toks[1] or 'help'
	local fn = H_[verb]
	local args = R.label(table.concat(toks, ' ', 2))
	if fn == nil then
		R.row('cmd', { who = who, verb = 'unknown', ok = false, args = R.label(verb) })
		reply(pl, 'unknown verb ' .. R.label(verb):sub(1, 32) .. '; verbs: ' .. table.concat(M.verbs, ' '))
		return false
	end
	local ok, res = pcall(fn, pl, toks)
	R.row('cmd', { who = who, verb = verb, ok = ok and res ~= false, args = args ~= '' and args or nil, via = via })
	if not ok then
		R.row('err', { where = 'cmd.' .. verb, msg = R.label(tostring(res)):sub(1, 120), n = 1 })
		reply(pl, verb .. ' failed (error logged)')
		return false
	end
	return res ~= false
end

-- on_chat(player, recipientMask, message): only '!mh' messages from humans.
function M.on_chat(pl, mask, message)
	if pl == nil or type(message) ~= 'string' then return end
	if bots.owns(pl) then return end
	if message:sub(1, 3) ~= '!mh' then return end
	local rest = message:sub(4)
	if rest ~= '' and rest:sub(1, 1) ~= ' ' then return end
	M.handle(pl, rest, 'chat')
end

return M

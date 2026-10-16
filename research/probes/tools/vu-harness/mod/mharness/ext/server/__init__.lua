-- mharness server entry (DESIGN.md section 3). Wires events, hooks, NetEvents and commands.
-- Events:Subscribe (shared/library/events.md), Hooks:Install (shared/library/hooks.md), NetEvents
-- (server/library/netevents.md). UpdateManager:Update with UpdatePass_PreSim / PostSim
-- (shared/event/updatemanager_update.md, fb/updatepass.md); Engine:Update fallback clock (shared/event/engine_update.md)
-- [live:L03]. Level:Loaded (server/event/level_loaded.md), Level:Destroy (shared/event/level_destroy.md),
-- Player:Chat (server/event/player_chat.md), Player:Authenticated (server/event/player_authenticated.md).
-- Observe-only: hooks never call Pass or Return (content/vext/guides/hooks.md); fixtures only in mh_bots.
-- ext/shared/__init__.lua also runs on its own in each realm; the shared modules are required here explicitly
-- (require caches by path, so loading twice is harmless) and the load order between the two entries does not matter.
MH = MH or {}
local R = require('__shared/mh_row')
local C = require('__shared/mh_clock')
MH.row = R
MH.clock = C
MH.vec = require('__shared/mh_vec')
MH.gs = require('__shared/mh_gs')
R.realm = 'S'
R.clock = C
MH.ids = require('mh_ids')
MH.chat = require('mh_chat')
MH.meta = require('mh_meta')
MH.bots = require('mh_bots')
MH.sites = require('mh_sites')
MH.samplers = require('mh_samplers')
MH.hooks = require('mh_hooks')
MH.events = require('mh_events')
MH.runner = require('mh_runner')
MH.cmd = require('mh_cmd')
MH.scenarios = require('scenarios/registry')

local ids, chat, meta, bots, sites = MH.ids, MH.chat, MH.meta, MH.bots, MH.sites
local S, H, EV, RUN, CMD = MH.samplers, MH.hooks, MH.events, MH.runner, MH.cmd

local state = { pre_seen = 0, post_seen = 0, eng_n = 0, first = true, greeted = {} }

R.on_disable = function(where)
	chat.say(nil, 'handler ' .. where .. ' disabled after 20 errors')
end

local function vm_probe()
	-- census.vm (DESIGN 3.1, live:L27): the shared entry's marker is visible here only if both share one Lua state.
	if MH._shared_entry == true then return 'same' end
	return 'separate'
end

local function greet(pl)
	local sid = ids.player(pl)
	if sid == nil or state.greeted[sid] then return end
	state.greeted[sid] = true
	chat.say(pl, string.format('mharness %s ready (console: %s). !mh help', R.VERSION, meta.client_cmd or 'mh'))
end

-- One sim tick of work (PostSim, or Engine:Update in the fallback mode).
-- The clock (k) advances at PreSim (C.mode 'presim'), so the hook, GunSway and PostSim rows of one sim tick share k;
-- fallbacks: 'postsim' if PreSim never fires, 'engine' if neither UpdateManager pass fires [live:L03].
local function tick_work(dt)
	if state.first then
		state.first = false
		meta.vm = vm_probe()
		meta.session_meta(RUN)
	end
	S.on_postsim()
	H.on_postsim()
	sites.tick()
	RUN.step()
	if bots.pass == 'postsim' then bots.apply_inputs(dt) end
end

Events:Subscribe('UpdateManager:Update', function(deltaTime, updatePass)
	if updatePass == UpdatePass.UpdatePass_PreSim then
		state.pre_seen = state.pre_seen + 1
		if C.mode == 'presim' then C.on_tick(deltaTime) end
		if bots.pass == 'presim' then R.safe('presim', bots.apply_inputs, deltaTime) end
	elseif updatePass == UpdatePass.UpdatePass_PostSim then
		state.post_seen = state.post_seen + 1
		if C.mode == 'presim' and state.pre_seen == 0 and state.post_seen > 30 then
			C.mode = 'postsim'
			R.row('mark', { lbl = 'clock-postsim' })
		end
		if C.mode == 'postsim' then C.on_tick(deltaTime) end
		if C.mode ~= 'engine' then R.safe('postsim', tick_work, deltaTime) end
	end
end)

Events:Subscribe('Engine:Update', function(deltaTime, simulationDeltaTime)
	state.eng_n = state.eng_n + 1
	if C.mode ~= 'engine' and state.post_seen == 0 and state.pre_seen == 0 and state.eng_n > 90 then
		C.mode = 'engine'
		R.row('mark', { lbl = 'clock-engine' })
	end
	if C.mode == 'engine' and type(simulationDeltaTime) == 'number' and simulationDeltaTime > 0 then
		C.on_tick(simulationDeltaTime)
		R.safe('engine', tick_work, simulationDeltaTime)
	end
end)

Events:Subscribe('Level:Loaded', function(levelName, gameMode, round, roundsPerMap)
	R.safe('level.loaded', function()
		R.row('ev', { name = 'Level:Loaded', a = { lvl = R.label(levelName), mode = R.label(gameMode),
			rnd = math.tointeger(round) or 0, rpm = math.tointeger(roundsPerMap) or 0 } })
		RUN.on_level_loaded(levelName, gameMode, round, roundsPerMap)
		meta.session_meta(RUN)
	end)
end)

Events:Subscribe('Level:Destroy', function()
	R.safe('level.destroy', RUN.on_level_destroy)
end)

Events:Subscribe('Player:Chat', function(player, recipientMask, message)
	R.safe('chat', CMD.on_chat, player, recipientMask, message)
end)

Events:Subscribe('Player:Authenticated', function(player)
	R.safe('auth', function()
		if bots.owns(player) then return end
		ids.player(player)
		RUN.on_human_seen()
		NetEvents:SendTo('mh:ping', player, SharedUtils:GetTimeMS())
	end)
end)

EV.on('Player:Respawn', function(player)
	if not bots.owns(player) then greet(player) end
end)

EV.on('Player:Left', function(player, sid)
	RUN.on_human_left()
end)

NetEvents:Subscribe('mh:cmd', function(player, text)
	R.safe('netcmd', function()
		if bots.owns(player) or type(text) ~= 'string' or #text > 400 then return end
		CMD.handle(player, text, 'console')
	end)
end)

NetEvents:Subscribe('mh:cam', function(player, msg)
	R.safe('netcam', sites.on_cam, player, msg)
end)

NetEvents:Subscribe('mh:hello', function(player, msg)
	R.safe('nethello', function()
		if type(msg) == 'table' and type(msg.cmd) == 'string' then meta.client_cmd = R.label(msg.cmd):sub(1, 64) end
		ids.player(player)
		RUN.on_human_seen()
		greet(player)
		NetEvents:SendTo('mh:ping', player, SharedUtils:GetTimeMS())
	end)
end)

NetEvents:Subscribe('mh:pong', function(player, msg)
	R.safe('netpong', function()
		if type(msg) ~= 'table' or type(msg.s) ~= 'number' or type(msg.c) ~= 'number' then return end
		local now = SharedUtils:GetTimeMS()
		local rtt = now - msg.s
		R.row('p0', { step = 'ping', ok = true, val = { rtt = rtt, off = msg.c - (msg.s + rtt / 2), id = ids.player(player) } })
	end)
end)

Events:Subscribe('GunSway:Update', function(gunSway, weapon, weaponFiring, deltaTime)
	R.safe('gunsway', S.on_gunsway, gunSway, weapon, weaponFiring, deltaTime)
end)

H.install()
EV.subscribe()

C.ensure_session()
R.plain('mharness ' .. R.VERSION .. ' loaded (server)')
meta.join_line()

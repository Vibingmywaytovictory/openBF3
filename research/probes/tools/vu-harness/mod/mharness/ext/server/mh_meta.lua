-- meta and census rows (DESIGN.md sections 4, 6.1).
-- Level:Loaded (server/event/level_loaded.md); SharedUtils:GetTickrate/GetLevelName/GetCurrentGameMode
-- (shared/library/sharedutils.md); TicketManager:GetCurrentRound/GetRoundCount (server/library/ticketmanager.md);
-- RCON:SendCommand with whitelisted literal read commands only (server/library/rcon.md, apicheck rule 11)
-- [live:L04 L05 L35]; RCON:GetServerGuid [live:L18 L28]; census via EntityManager:TraverseAllEntities
-- (shared/library/entitymanager.md). The password variable is never read.
local R = require('__shared/mh_row')
local C = require('__shared/mh_clock')
local ids = require('mh_ids')
local bots = require('mh_bots')
local W = require('scenarios/wa_cfg')
local INV = require('mh_inventory')

local M = {}

M.client_cmd = nil  -- full name of the client console command (mh:hello)
M.vm = 'unknown'
M.cached = nil
M.build = nil

local function val(res)
	if type(res) ~= 'table' then return nil end
	if res[1] ~= 'OK' then return nil end
	local v = res[2]
	if v == nil then return true end
	if v == 'true' then return true end
	if v == 'false' then return false end
	local n = tonumber(v)
	if n then return n end
	return R.label(v)
end
M.val = val

-- Whitelisted reads (one literal per call: apicheck rule 11). Never vars.gamePassword.
function M.read_vars()
	local v = {}
	local function put(key, res)
		local x = val(res)
		if x ~= nil then v[key] = x end
	end
	put('vars.soldierHealth', RCON:SendCommand('vars.soldierHealth'))
	put('vars.regenerateHealth', RCON:SendCommand('vars.regenerateHealth'))
	put('vars.bulletDamage', RCON:SendCommand('vars.bulletDamage'))
	put('vars.playerRespawnTime', RCON:SendCommand('vars.playerRespawnTime'))
	put('vars.gameModeCounter', RCON:SendCommand('vars.gameModeCounter'))
	put('vars.friendlyFire', RCON:SendCommand('vars.friendlyFire'))
	put('vars.autoBalance', RCON:SendCommand('vars.autoBalance'))
	put('vars.idleTimeout', RCON:SendCommand('vars.idleTimeout'))
	put('vars.roundStartPlayerCount', RCON:SendCommand('vars.roundStartPlayerCount'))
	put('vars.vehicleSpawnAllowed', RCON:SendCommand('vars.vehicleSpawnAllowed'))
	put('vu.SuppressionMultiplier', RCON:SendCommand('vu.SuppressionMultiplier'))
	put('vu.TimeScale', RCON:SendCommand('vu.TimeScale'))
	put('vu.DisablePreRound', RCON:SendCommand('vu.DisablePreRound'))
	return v
end

function M.freq()
	local ok, res = pcall(function() return RCON:SendCommand('vu.FrequencyMode') end)
	local x = ok and val(res) or nil
	if type(x) == 'string' then return x end
	return 'unknown'
end

function M.mods()
	local ok, res = pcall(function() return RCON:SendCommand('modList.ListRunning') end)
	local out = {}
	if ok and type(res) == 'table' and res[1] == 'OK' then
		for i = 2, #res do out[#out + 1] = R.label(res[i]) end
	end
	if #out == 0 then return R.EMPTY end
	return out
end

-- build: the VU build from RCON 'version' (live:L35): {'OK', 'VU', '<build>'} assumed; the raw table shape is logged.
function M.read_build()
	local ok, res = pcall(function() return RCON:SendCommand('version') end)
	if ok and type(res) == 'table' then
		for i = 2, #res do
			local n = tonumber(res[i])
			if n and math.tointeger(n) then
				M.build = math.tointeger(n)
				return M.build
			end
		end
	end
	return nil
end

local function safe_call(fn, default)
	local ok, v = pcall(fn)
	if ok and v ~= nil then return v end
	return default
end

function M.meta_payload(args, runner)
	local wpn = {}
	for wid, name in pairs(ids.wnames) do wpn[tostring(wid)] = { name = name } end
	local cal = bots.cal and { sx = bots.cal.sx, sz = bots.cal.sz, off = bots.cal.off, unit = bots.cal.unit,
		ps = bots.cal.ps, src = bots.cal_src, dt = bots.last_dt, pass = bots.pass } or nil
	return {
		hv = R.VERSION,
		lvl = R.label(safe_call(function() return SharedUtils:GetLevelName() end, 'unknown')),
		mode = R.label(safe_call(function() return SharedUtils:GetCurrentGameMode() end, 'unknown')),
		rnd = math.tointeger(safe_call(function() return TicketManager:GetCurrentRound() end, 0)) or 0,
		rpm = math.tointeger(safe_call(function() return TicketManager:GetRoundCount() end, 0)) or 0,
		hz = safe_call(function() return SharedUtils:GetTickrate() end, 0) + 0.0,
		freq = M.freq(),
		vars = safe_call(M.read_vars, {}),
		mods = M.mods(),
		clk = C.mode,
		aimcal = cal,
		args = args,
		build = M.build,
		wpn = next(wpn) and wpn or nil,
		tcost = runner and runner.tcost or nil,
		fmt = '9g',
	}
end

function M.session_meta(runner)
	if M.build == nil then M.read_build() end
	R.row('meta', M.meta_payload(nil, runner))
end

function M.run_meta(sc, args, runner)
	local a = {}
	for key, v in pairs(args or {}) do a[R.label(key)] = type(v) == 'string' and R.label(v) or v end
	R.row('meta', M.meta_payload(next(a) and a or nil, runner))
end

-- join line: a plain log line (not a row; the extractor never copies it) with the GUID with and without dashes.
function M.join_line(password_hint)
	local ok, g = pcall(function() return RCON:GetServerGuid() end)
	if not ok or g == nil then
		R.plain('join: no server GUID (RCON:GetServerGuid nil); see the RUNBOOK failure table')
		return false
	end
	local s = tostring(g)
	local nodash = s:gsub('[{}%-]', '')
	R.plain('join with the client console: connect ' .. nodash .. ' <password>   (vu://join/' .. nodash ..
		'/<password>; raw form ' .. s .. ')')
	return s:find('-', 1, true) == nil
end

-- client_ctl(player, rec): start (rec table) or stop (nil) client recording over NetEvents 'mh:ctl'.
function M.client_ctl(pl, rec)
	if pl == nil then return end
	local msg = { run = C.run_id(), sc = C.scen(), on = rec ~= nil, gs = rec and rec.gs or false,
		fov = rec and rec.fov or false }
	NetEvents:SendTo('mh:ctl', pl, msg)
end

-- Census (P0.2): API presence for every inventory symbol the mod uses, entity type counts, assets, json sample,
-- os.date, vm. Every presence probe is wrapped in pcall (reading an absent member may raise, DESIGN 3.1).
local function probe_api(pl)
	local api = {}
	local so = nil
	pcall(function() so = pl and pl.soldier end)
	local objs = { Player = pl, SoldierEntity = so }
	pcall(function()
		local w = so.weaponsComponent
		objs.SoldierWeaponsComponent = w
		objs.SoldierWeapon = w.currentWeapon
		objs.WeaponFiring = w.currentWeapon.weaponFiring
		objs.GunSway = w.currentWeapon.weaponFiring.gunSway
	end)
	pcall(function() objs.LinearTransform = so.worldTransform end)
	pcall(function() objs.Vec3 = so.worldTransform.trans end)
	pcall(function() objs.EntryInput = EntryInput() end)
	pcall(function()
		local it = EntityManager:GetIterator('ServerCapturePointEntity')
		local e = it:Next()
		if e then objs.CapturePointEntity = CapturePointEntity(e) end
	end)
	pcall(function()
		local it = EntityManager:GetIterator('ServerTicketCounterEntity')
		local e = it:Next()
		if e then objs.TicketCounterEntity = TicketCounterEntity(e) end
	end)
	local libs = { Events = Events, Hooks = Hooks, NetEvents = NetEvents, ChatManager = ChatManager,
		PlayerManager = PlayerManager, EntityManager = EntityManager, SharedUtils = SharedUtils, RCON = RCON,
		TicketManager = TicketManager, RaycastManager = RaycastManager, ResourceManager = ResourceManager }
	local enums = { EntryInputActionEnum = EntryInputActionEnum, CharacterPoseType = CharacterPoseType,
		UpdatePass = UpdatePass, WeaponSlot = WeaponSlot, TeamId = TeamId, SquadId = SquadId,
		RayCastFlags = RayCastFlags, EntryInputFlags = EntryInputFlags, WeaponState = WeaponState,
		FireLogicType = FireLogicType }
	for _, e in ipairs(INV.server) do
		local kind, sym = e[1], e[2]
		local present = nil
		if kind == 'method' then
			local lib, m = sym:match('^(%w+):(%w+)$')
			if lib and libs[lib] then
				present = select(2, pcall(function() return libs[lib][m] ~= nil end)) == true
			end
		elseif kind == 'prop' then
			local t, f = sym:match('^(%w+)%.(%w+)$')
			if t and objs[t] ~= nil then
				present = select(2, pcall(function() return objs[t][f] ~= nil end)) == true
			end
		elseif kind == 'enum' then
			local t, v = sym:match('^(%w+)%.([%w_]+)$')
			if t and enums[t] then
				present = select(2, pcall(function() return enums[t][v] ~= nil end)) == true
			end
		end
		if present ~= nil then api[sym] = present end
	end
	return api
end

-- chunk_rows(tbl): split into parts of at most 40 keys and about 650 bytes each (rows stay under 1000 B).
local function chunk_rows(tbl)
	local keys = {}
	for key in pairs(tbl) do keys[#keys + 1] = key end
	table.sort(keys)
	local parts = {}
	local cur, n, size = {}, 0, 0
	for _, key in ipairs(keys) do
		local lk = R.label(key)
		local add = #lk + 12
		if n > 0 and (n >= 40 or size + add > 650) then
			parts[#parts + 1] = cur
			cur, n, size = {}, 0, 0
		end
		cur[lk] = tbl[key]
		n = n + 1
		size = size + add
	end
	if n > 0 or #parts == 0 then parts[#parts + 1] = cur end
	return parts
end

function M.census(pl)
	local api = probe_api(pl)
	local types = {}
	local ntypes = 0
	pcall(function()
		EntityManager:TraverseAllEntities(function(e)
			local tn = 'unknown'
			pcall(function() tn = e.typeInfo.name end)
			if types[tn] == nil then ntypes = ntypes + 1 end
			types[tn] = (types[tn] or 0) + 1
		end)
	end)
	local res = {}
	local function try(list)
		local dc, name = bots.find(list)
		res[list[1]] = dc ~= nil
		return dc ~= nil, name
	end
	for _, w in ipairs({ 'M16A3', 'AK74M', 'M416', 'M249', 'M98B', 'M9', '870MCS', 'RPG7', 'SMAW', 'M320HE', 'M67' }) do
		try(W.unlocks[w])
	end
	try(W.soldier_blueprint)
	try(W.kits.team1)
	try(W.kits.team2)
	try(W.appearance.team1)
	try(W.appearance.team2)
	local jsonv3 = 'error'
	pcall(function() jsonv3 = R.label(json.encode(Vec3(1, 2, 3))) end)
	local osdate = pcall(function() return os.date('%Y%m%d') end)
	local guid_nodash = nil
	pcall(function() guid_nodash = tostring(RCON:GetServerGuid()):find('-', 1, true) == nil end)
	local parts = {}
	for _, p in ipairs(chunk_rows(api)) do parts[#parts + 1] = { api = p } end
	for _, p in ipairs(chunk_rows(types)) do parts[#parts + 1] = { types = p } end
	for _, p in ipairs(chunk_rows(res)) do parts[#parts + 1] = { res = p } end
	local n = #parts
	for i, p in ipairs(parts) do
		R.row('census', { part = i, of = n, api = p.api, types = p.types, res = p.res, jsonv3 = i == 1 and jsonv3 or nil,
			osdate = i == 1 and osdate or nil, vm = i == 1 and M.vm or nil, guid_nodash = i == 1 and guid_nodash or nil,
			cmd = i == 1 and M.client_cmd and R.label(M.client_cmd) or nil })
	end
	local na, np = 0, 0
	for _, v in pairs(api) do
		na = na + 1
		if v then np = np + 1 end
	end
	local nr, nf = 0, 0
	for _, v in pairs(res) do
		nr = nr + 1
		if v then nf = nf + 1 end
	end
	local flags, tcs = 0, 0
	pcall(function()
		local it = EntityManager:GetIterator('ServerCapturePointEntity')
		while it:Next() do flags = flags + 1 end
	end)
	pcall(function()
		local it = EntityManager:GetIterator('ServerTicketCounterEntity')
		while it:Next() do tcs = tcs + 1 end
	end)
	local missing = {}
	for key, v in pairs(res) do if not v then missing[#missing + 1] = key:match('[^/]+$') end end
	table.sort(missing)
	return { api = np, apin = na, res = nf, resn = nr, types = ntypes, flags = flags, tcs = tcs, vm = M.vm,
		cmd = M.client_cmd or '?', missing = missing, osdate = osdate, jsonv3 = jsonv3 }
end

return M

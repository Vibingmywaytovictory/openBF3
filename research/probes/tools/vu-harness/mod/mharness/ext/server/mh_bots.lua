-- Test-bot fixtures: the ONLY module that writes game state (DESIGN.md section 3.9; R section 3.2 rule 3).
-- Every fixture emits an 'fx' row and refuses players it did not create.
-- PlayerManager:CreatePlayer/DeletePlayer (server/library/playermanager.md); Player:SelectWeapon/SelectUnlockAssets/
-- CreateSoldier/SpawnSoldierAt/EnterVehicle (server/type/player.md); SoldierEntity:SetPosition/SetTransform/SetPose/
-- Kill (server/type/soldierentity.md); EntryInput (shared/type/entryinput.md, shared/type/entryinputflags.md);
-- LinearTransform(left, up, forward, trans) (shared/type/lineartransform.md);
-- ResourceManager:SearchForDataContainer (shared/library/resourcemanager.md, read only).
-- [live:L07 L08 L09 L24 L29 L30 L31]
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local ids = require('mh_ids')
local W = require('scenarios/wa_cfg')

local M = {}

M.bots = {}        -- sid -> bot record
M.by_pid = {}      -- Player.id -> bot record
M.next_sid = 101
M.cal = nil        -- aim convention from P0.5 (mh_vec.dir_to_aim)
M.cal_src = 'none'
M.res = {}         -- asset name -> DataContainer | false
M.fxpos = {}       -- sid -> last fixture position (spawn/place), for dmg.fxd
M.counts = { [1] = { s = 0, d = 0, r = 0 }, [2] = { s = 0, d = 0, r = 0 } }
M.pass = 'presim'  -- input pass (L08): 'presim' or 'postsim'
M.last_dt = 1 / 30

local EYE = { [0] = 1.6, [1] = 1.0, [2] = 0.4 } -- eye height per pose [inferred]
M.EYE = EYE

function M.fx(act, sid, ok, o)
	o = o or {}
	R.row('fx', { act = act, id = sid or 0, ok = ok == true, pos = o.pos, yaw = o.yaw, pitch = o.pitch, pose = o.pose,
		team = o.team, wid = o.wid, prof = o.prof, arg = o.arg, slot = o.slot, fl = o.fl, dt = o.dt })
end

-- find(names): the first data container that resolves (read only); cached.
function M.find(names)
	if type(names) == 'string' then names = { names } end
	for _, n in ipairs(names or {}) do
		local hit = M.res[n]
		if hit == nil then
			local ok, dc = pcall(function() return ResourceManager:SearchForDataContainer(n) end)
			hit = (ok and dc ~= nil) and dc or false
			M.res[n] = hit
		end
		if hit then return hit, n end
	end
	return nil, nil
end

function M.owns(pl)
	if pl == nil then return false end
	local b = M.by_pid[pl.id]
	return b ~= nil and b.pl == pl
end

local function mine(bot)
	if bot == nil or M.bots[bot.sid] ~= bot or not M.owns(bot.pl) then
		R.row('err', { where = 'bots.owner', msg = 'refused: not a harness bot', n = 1 })
		return false
	end
	return true
end

function M.soldier(bot)
	if bot == nil or bot.gone then return nil end
	local ok, so = pcall(function() return bot.pl.soldier end)
	if ok then return so end
	return nil
end

function M.alive(bot)
	local so = M.soldier(bot)
	if so == nil then return false end
	local ok, a = pcall(function() return so.isAlive end)
	return ok and a == true
end

function M.pos(bot)
	local so = M.soldier(bot)
	if so == nil then return nil end
	return V.v(so.worldTransform.trans)
end

function M.pose_of(bot)
	local so = M.soldier(bot)
	if so == nil then return 0 end
	return math.tointeger(so.pose) or 0
end

function M.eye(bot)
	local p = M.pos(bot)
	if p == nil then return nil end
	return { p[1], p[2] + EYE[M.pose_of(bot)], p[3] }
end

function M.weapon(bot)
	local so = M.soldier(bot)
	if so == nil then return nil end
	local ok, w = pcall(function() return so.weaponsComponent.currentWeapon end)
	if ok then return w end
	return nil
end

function M.slot_of(bot)
	local so = M.soldier(bot)
	if so == nil then return -1 end
	local ok, s = pcall(function() return so.weaponsComponent.currentWeaponSlot end)
	if ok then return math.tointeger(s) or -1 end
	return -1
end

function M.fire_logic(bot)
	local w = M.weapon(bot)
	if w == nil then return -1 end
	local ok, f = pcall(function() return w.fireLogic end)
	if ok then return math.tointeger(f) or -1 end
	return -1
end

function M.wid(bot)
	local w = M.weapon(bot)
	if w == nil then return 0 end
	return ids.weapon(w.name)
end

function M.health(bot)
	local so = M.soldier(bot)
	if so == nil then return 0 end
	return so.health
end

local function transform(pos, dir)
	local f = V.horiz(dir or { 0, 0, 1 })
	if V.len(f) < 0.5 then f = { 0, 0, 1 } end
	local up = { 0, 1, 0 }
	local left = V.cross(up, f)
	return LinearTransform(V.vec3(left), V.vec3(up), V.vec3(f), V.vec3(pos))
end

local function kit_for(team)
	return M.find(team == 1 and W.kits.team1 or W.kits.team2)
end

-- create(team, w, opt): CreatePlayer + equip; opt.secondary = weapon key for slot 1 (P0.6 slot test).
function M.create(team, w, opt)
	opt = opt or {}
	local sid = M.next_sid
	M.next_sid = sid + 1
	if M.next_sid > 999 then M.next_sid = 101 end
	local pl = PlayerManager:CreatePlayer('mh' .. sid, team, SquadId.SquadNone)
	if pl == nil then
		M.fx('create', sid, false, { team = team })
		return nil
	end
	local bot = { sid = sid, pl = pl, tm = team, w = w, wslot = W.slot[w] or 0, lv = {}, pr = {}, yawv = 0.0,
		pitchv = 0.0, zoomv = 0, inp = nil, prof = 'idle', dirty = true, born = C.gk }
	M.bots[sid] = bot
	M.by_pid[pl.id] = bot
	ids.register_bot(pl, sid, team, team, 0)
	M.fx('create', sid, true, { team = team })
	M.equip(bot, w, opt.secondary)
	return bot
end

function M.equip(bot, w, secondary)
	if not mine(bot) then return false end
	local unlock, uname = M.find(W.unlocks[w] or {})
	local kit = kit_for(bot.tm)
	local ok = unlock ~= nil
	local slot = W.slot[w] or 0
	if unlock then
		bot.pl:SelectWeapon(WeaponSlot['WeaponSlot_' .. slot], unlock, {})
	end
	if secondary then
		local u2 = M.find(W.unlocks[secondary] or {})
		if u2 then bot.pl:SelectWeapon(WeaponSlot['WeaponSlot_' .. (W.slot[secondary] or 1)], u2, {}) end
	end
	if kit then bot.pl.selectedKit = kit end
	local app = M.find(bot.tm == 1 and W.appearance.team1 or W.appearance.team2)
	if app then bot.pl:SelectUnlockAssets(app, {}) end
	bot.w = w
	bot.wslot = slot
	local wid = ids.weapon(uname or w)
	bot.uwid = wid
	M.fx('equip', bot.sid, ok, { wid = wid, slot = slot, team = bot.tm, arg = { kit = kit ~= nil, app = app ~= nil } })
	return ok
end

-- spawn(bot, pos, dir, pose): CreateSoldier + SpawnSoldierAt (a ticket in Conquest).
function M.spawn(bot, pos, dir, pose)
	if not mine(bot) then return false end
	pose = pose or 0
	local bp = M.find(W.soldier_blueprint)
	if bp == nil then
		M.fx('spawn', bot.sid, false, { pos = pos, arg = { why = 'no blueprint' } })
		return false
	end
	local tr = transform(pos, dir)
	local so = bot.pl:CreateSoldier(bp, tr)
	if so == nil then
		M.fx('spawn', bot.sid, false, { pos = pos, arg = { why = 'CreateSoldier nil' } })
		return false
	end
	bot.pl:SpawnSoldierAt(so, tr, pose)
	local c = M.counts[bot.tm]
	if c then c.s = c.s + 1 end
	M.fxpos[bot.sid] = pos
	bot.spawn_k = C.gk
	if dir then M.face(bot, dir) end
	M.fx('spawn', bot.sid, true, { pos = pos, pose = pose, team = bot.tm })
	return true
end

-- place(bot, pos, dir): teleport (no ticket). act 'bench' when parking a bot out of play.
function M.place(bot, pos, dir, act)
	if not mine(bot) then return false end
	local so = M.soldier(bot)
	if so == nil then
		M.fx(act or 'place', bot.sid, false, { pos = pos })
		return false
	end
	if dir then
		so:SetTransform(transform(pos, dir))
		M.face(bot, dir)
	else
		so:SetPosition(V.vec3(pos))
	end
	M.fxpos[bot.sid] = pos
	M.fx(act or 'place', bot.sid, true, { pos = pos })
	return true
end

function M.bench(bot, pos, dir)
	return M.place(bot, pos, dir, 'bench')
end

function M.pose(bot, pose)
	if not mine(bot) then return false end
	local so = M.soldier(bot)
	if so == nil then
		M.fx('pose', bot.sid, false, { pose = pose })
		return false
	end
	so:SetPose(pose, true, true)
	M.fx('pose', bot.sid, true, { pose = pose })
	return true
end

-- Aim. yaw/pitch are raw input values in the calibrated unit; aim_dir needs the P0.5 convention.
function M.aim(bot, yaw, pitch)
	bot.yawv = yaw
	bot.pitchv = pitch
end

function M.face(bot, dir)
	if M.cal == nil then return false end
	local y, p = V.dir_to_aim(M.cal, dir)
	bot.yawv = y
	bot.pitchv = p
	return true
end

function M.aim_dir(bot, dir)
	return M.face(bot, dir)
end

function M.aim_at(bot, target)
	local e = M.eye(bot)
	if e == nil then return false end
	return M.face(bot, V.sub(target, e))
end

function M.zoom(bot, on)
	bot.zoomv = on and 1 or 0
	M.hold(bot, EntryInputActionEnum.EIAZoom, on and 1.0 or 0.0)
end

function M.hold(bot, action, level)
	bot.lv[action] = level
	bot.pr[action] = nil
end

function M.release(bot, action)
	if bot.lv[action] ~= nil then bot.lv[action] = 0.0 end
	bot.pr[action] = nil
end

-- press(bot, action, ticks): level 1 for n input passes, then released.
function M.press(bot, action, ticks)
	bot.lv[action] = 1.0
	bot.pr[action] = ticks or 1
end

function M.release_all(bot)
	for a in pairs(bot.lv) do bot.lv[a] = 0.0 end
	bot.pr = {}
	bot.zoomv = 0
end

function M.profile(bot, name, arg)
	if bot.prof ~= name then
		bot.prof = name
		bot.parg = arg
		bot.dirty = true
	end
end

-- apply_inputs(dt): every input pass sets input.deltaTime = dt (DESIGN 3.2, live:L08) before assigning player.input.
function M.apply_inputs(dt)
	if type(dt) ~= 'number' or dt <= 0 then dt = M.last_dt end
	M.last_dt = dt
	for _, bot in pairs(M.bots) do
		if not bot.gone and M.soldier(bot) ~= nil then
			local inp = bot.inp
			if inp == nil then
				inp = EntryInput()
				bot.inp = inp
			end
			inp.deltaTime = dt
			inp.flags = EntryInputFlags.AuthoritativeAiming
			inp.authoritativeAimingYaw = bot.yawv
			inp.authoritativeAimingPitch = bot.pitchv
			inp.zoomLevel = bot.zoomv
			for a, l in pairs(bot.lv) do inp:SetLevel(a, l) end
			bot.pl.input = inp
			for a, n in pairs(bot.pr) do
				if n <= 1 then
					bot.pr[a] = nil
					bot.lv[a] = 0.0
				else
					bot.pr[a] = n - 1
				end
			end
			if bot.dirty then
				bot.dirty = false
				M.fx('input', bot.sid, true, { prof = bot.prof, dt = dt, arg = bot.parg })
			end
		end
	end
end

-- select_slot(bot, slot): EIASelectWeapon<slot+1> for 1 tick (fb/entryinputactionenum.md) [live:L31].
function M.select_slot(bot, slot)
	local a = EntryInputActionEnum['EIASelectWeapon' .. (slot + 1)]
	M.press(bot, a, 2)
	M.fx('input', bot.sid, true, { slot = slot, arg = { sel = slot } })
end

-- cycle_fire(bot): EIACycleFireMode for 1 tick [live:L30]. fireLogic itself is never assigned.
function M.cycle_fire(bot)
	M.press(bot, EntryInputActionEnum.EIACycleFireMode, 2)
	M.fx('input', bot.sid, true, { fl = M.fire_logic(bot), arg = { cyc = 1 } })
end

function M.kill(bot)
	if not mine(bot) then return false end
	local so = M.soldier(bot)
	if so == nil then
		M.fx('kill', bot.sid, false)
		return false
	end
	so:Kill()
	local c = M.counts[bot.tm]
	if c then c.d = c.d + 1 end
	M.fx('kill', bot.sid, true)
	return true
end

function M.remove(bot)
	if not mine(bot) then return false end
	local pl = bot.pl
	M.bots[bot.sid] = nil
	M.by_pid[pl.id] = nil
	ids.forget(pl)
	bot.gone = true
	PlayerManager:DeletePlayer(pl)
	local c = M.counts[bot.tm]
	if c then c.r = c.r + 1 end
	M.fx('remove', bot.sid, true)
	return true
end

function M.remove_all()
	local list = {}
	for _, b in pairs(M.bots) do list[#list + 1] = b end
	table.sort(list, function(a, b) return a.sid < b.sid end)
	for _, b in ipairs(list) do M.remove(b) end
	return #list
end

function M.count()
	local n = 0
	for _ in pairs(M.bots) do n = n + 1 end
	return n
end

-- seat(bot, vehicle, entryId): Sessions 2-3 extension point (Player:EnterVehicle, server/type/player.md).
function M.seat(bot, vehicle, entryId)
	if not mine(bot) then return false end
	bot.pl:EnterVehicle(vehicle, entryId)
	M.fx('seat', bot.sid, true, { arg = { entry = entryId } })
	return true
end

-- Level destroyed: soldiers are gone; the players may or may not survive (L22).
function M.on_level_destroy()
	for _, b in pairs(M.bots) do b.inp = nil end
end

return M

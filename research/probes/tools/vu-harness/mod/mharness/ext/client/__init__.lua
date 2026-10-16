-- mharness client entry (DESIGN.md section 3.11). Rows only while the server's 'mh:ctl' says record.
-- NetEvents (client/library/netevents.md), Console:Register (client/library/console.md; ConsoleCommand.groupName/name,
-- client/type/consolecommand.md) [live:L19], Engine:Update (shared/event/engine_update.md) as the frame clock,
-- GunSway:Update (shared/event/gunsway_update.md), Level:Loaded (client/event/level_loaded.md).
-- ext/shared/__init__.lua also runs on its own in each realm; the shared modules are required here explicitly
-- (require caches by path, so loading twice is harmless) and the load order between the two entries does not matter.
MH = MH or {}
local R = require('__shared/mh_row')
local C = require('__shared/mh_clock')
MH.row = R
MH.clock = C
MH.vec = require('__shared/mh_vec')
MH.gs = require('__shared/mh_gs')
R.realm = 'C'
R.clock = C
local cgs = require('mh_cgs')
local fov = require('mh_fov')
local cam = require('mh_cam')
MH.cgs, MH.fov, MH.cam = cgs, fov, cam

local rec = { on = false, gs = false, fov = false, id = 0 }
MH.rec = rec
local full_name = 'mh'
local hello_sent = false

local cmd = Console:Register('mh', 'mharness: forwards its arguments to the server as !mh', function(args)
	local parts = {}
	for i = 1, #(args or {}) do parts[#parts + 1] = tostring(args[i]) end
	NetEvents:Send('mh:cmd', table.concat(parts, ' '))
	return 'sent: mh ' .. table.concat(parts, ' ')
end)
pcall(function() full_name = cmd.groupName .. '.' .. cmd.name end)
R.plain('client console command: ' .. full_name .. ' (try "mh status", then "' .. full_name .. ' status")')

local function hello()
	if hello_sent then return end
	hello_sent = true
	NetEvents:Send('mh:hello', { cmd = full_name })
end

NetEvents:Subscribe('mh:ctl', function(msg)
	R.safe('ctl', function()
		if type(msg) ~= 'table' then return end
		if msg.on then
			C.adopt(msg.run, msg.sc)
			rec.on, rec.gs, rec.fov, rec.id = true, msg.gs == true, msg.fov == true, math.tointeger(msg.id) or 0
			fov.reset()
			cgs.reset()
		else
			rec.on, rec.gs, rec.fov = false, false, false
			C.end_run()
		end
	end)
end)

NetEvents:Subscribe('mh:say', function(text)
	if type(text) == 'string' then R.plain('chat: ' .. text:sub(1, 200)) end
end)

NetEvents:Subscribe('mh:camreq', function(tag)
	R.safe('camreq', cam.reply, tag)
end)

NetEvents:Subscribe('mh:ping', function(s)
	R.safe('ping', function()
		if type(s) == 'number' then NetEvents:Send('mh:pong', { s = s, c = SharedUtils:GetTimeMS() }) end
	end)
end)

Events:Subscribe('Level:Loaded', function(levelName, gameMode)
	R.safe('level', hello)
end)

Events:Subscribe('Engine:Update', function(deltaTime, simulationDeltaTime)
	R.safe('frame', function()
		C.on_tick(deltaTime)
		if not hello_sent and C.gk > 30 then hello() end
		if rec.on and rec.fov then fov.on_frame(rec) end
		cgs.frame = C.gk
	end)
end)

Events:Subscribe('GunSway:Update', function(gunSway, weapon, weaponFiring, deltaTime)
	if rec.on and rec.gs then R.safe('cgs', cgs.on_gunsway, gunSway, weapon, weaponFiring, deltaTime, rec) end
end)

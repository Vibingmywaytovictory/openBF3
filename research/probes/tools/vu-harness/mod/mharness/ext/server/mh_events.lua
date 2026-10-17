-- Event rows (DESIGN.md section 3.10). server/event/*.md and shared/event/level_destroy.md.
-- Player:Joining is never subscribed (its arguments carry name, GUID, IP and account GUID).
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local ids = require('mh_ids')
local bots = require('mh_bots')

local M = {}

M.listeners = {}   -- name -> list of fn(...) (other modules: runner, scenarios)
M.counts = {}

function M.on(name, fn)
	local l = M.listeners[name] or {}
	l[#l + 1] = fn
	M.listeners[name] = l
end

local function fire(name, ...)
	M.counts[name] = (M.counts[name] or 0) + 1
	for _, fn in ipairs(M.listeners[name] or {}) do R.safe('ev.' .. name, fn, ...) end
end
M.fire = fire

local function pid(pl)
	if pl == nil then return nil end
	return ids.player(pl)
end

local function cpid(cp)
	if cp == nil then return nil end
	local ok, e = pcall(function() return CapturePointEntity(cp) end)
	if ok and e then return ids.flag(e) end
	return nil
end

function M.subscribe()
	Events:Subscribe('Player:Killed', function(player, inflictor, position, weapon, isRoadKill, isHeadShot,
		wasVictimInReviveState, info)
		R.safe('ev.killed', function()
			local b = bots.by_pid[player.id]
			if b then
				local c = bots.counts[b.tm]
				if c then c.d = c.d + 1 end
			end
			R.row('ev', { name = 'Player:Killed', id = pid(player), id2 = pid(inflictor), pos = V.v(position),
				a = { hs = isHeadShot == true, rk = isRoadKill == true } })
			fire('Player:Killed', player, inflictor)
		end)
	end)
	Events:Subscribe('Player:Respawn', function(player)
		R.safe('ev.respawn', function()
			R.row('ev', { name = 'Player:Respawn', id = pid(player) })
			fire('Player:Respawn', player)
		end)
	end)
	Events:Subscribe('Player:Reload', function(player, weaponName, position)
		R.safe('ev.reload', function()
			R.row('ev', { name = 'Player:Reload', id = pid(player), wid = ids.weapon(weaponName), pos = V.v(position) })
		end)
	end)
	Events:Subscribe('Player:SuppressedEnemy', function(player, enemy)
		R.safe('ev.supp', function()
			R.row('ev', { name = 'Player:SuppressedEnemy', id = pid(player), id2 = pid(enemy) })
			fire('Player:SuppressedEnemy', player, enemy)
		end)
	end)
	Events:Subscribe('Player:EnteredCapturePoint', function(player, capturePoint)
		R.safe('ev.enter', function()
			R.row('ev', { name = 'Player:EnteredCapturePoint', id = pid(player), id2 = cpid(capturePoint) })
			fire('Player:EnteredCapturePoint', player, capturePoint)
		end)
	end)
	Events:Subscribe('Player:ExitedCapturePoint', function(player, capturePoint)
		R.safe('ev.exit', function()
			R.row('ev', { name = 'Player:ExitedCapturePoint', id = pid(player), id2 = cpid(capturePoint) })
		end)
	end)
	Events:Subscribe('Player:Left', function(player)
		R.safe('ev.left', function()
			local sid = ids.by_pid[player.id]
			local human = sid ~= nil and ids.is_human(sid)
			R.row('ev', { name = 'Player:Left', id = sid, a = { human = human } })
			if human then
				ids.forget(player)
				fire('Player:Left', player, sid)
			end
		end)
	end)
	Events:Subscribe('CapturePoint:Captured', function(capturePoint)
		R.safe('ev.captured', function()
			R.row('ev', { name = 'CapturePoint:Captured', id2 = cpid(capturePoint) })
		end)
	end)
	Events:Subscribe('CapturePoint:Lost', function(capturePoint)
		R.safe('ev.lost', function()
			R.row('ev', { name = 'CapturePoint:Lost', id2 = cpid(capturePoint) })
		end)
	end)
	Events:Subscribe('Soldier:HealthAction', function(soldier, action)
		R.safe('ev.health', function()
			R.row('ev', { name = 'Soldier:HealthAction', id = pid(soldier.player), a = { act = math.tointeger(action) or -1 } })
		end)
	end)
	Events:Subscribe('Soldier:ManDown', function(soldier, inflictor)
		R.safe('ev.mandown', function()
			R.row('ev', { name = 'Soldier:ManDown', id = pid(soldier.player) })
		end)
	end)
	Events:Subscribe('Server:RoundOver', function(roundTime, winningTeam)
		R.safe('ev.roundover', function()
			R.row('ev', { name = 'Server:RoundOver', a = { t = roundTime, win = math.tointeger(winningTeam) or -1 } })
			fire('Server:RoundOver')
		end)
	end)
	Events:Subscribe('Server:RoundReset', function()
		R.safe('ev.roundreset', function()
			R.row('ev', { name = 'Server:RoundReset' })
			fire('Server:RoundReset')
		end)
	end)
end

return M

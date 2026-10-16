-- Small integer ids (DESIGN.md section 3.4). Humans 1-99 in order of first sight, bots 101-999, weapons/materials/
-- flags 1.. . Never reads a human's name, guid, accountGuid, onlineId or ip. Emits 'dict' rows before the first use.
-- Player.id (server/type/player.md, RO int) is the key and is never logged.
local R = require('__shared/mh_row')

local M = {}

M.by_pid = {}     -- Player.id -> small id
M.players = {}    -- small id -> Player
M.humans = {}     -- small id -> true
M.next_human = 1
M.wids = {}       -- weapon data name -> wid
M.wnames = {}     -- wid -> name
M.next_wid = 1
M.mats = {}
M.next_mat = 1
M.cps = {}        -- CapturePointEntity instanceId -> cp id
M.cp_ent = {}     -- cp id -> entity
M.next_cp = 1
M.counter = 0     -- survives only while the Lua state does (P0.15)

function M.register_bot(pl, sid, team, kit, wid)
	M.by_pid[pl.id] = sid
	M.players[sid] = pl
	R.row('dict', { kind = 'bot', id = sid, name = 'mh' .. sid, x = { team = team, kit = kit or 0, wid = wid or 0 } })
end

function M.forget(pl)
	local sid = M.by_pid[pl.id]
	if sid then
		M.by_pid[pl.id] = nil
		M.players[sid] = nil
		M.humans[sid] = nil
	end
end

-- player(pl): the small id of a Player (assigns a human id on first sight). nil for nil.
function M.player(pl)
	if pl == nil then return nil end
	local pid = pl.id
	local sid = M.by_pid[pid]
	if sid then return sid end
	if M.next_human > 99 then return 0 end
	sid = M.next_human
	M.next_human = M.next_human + 1
	M.by_pid[pid] = sid
	M.players[sid] = pl
	M.humans[sid] = true
	return sid
end

function M.get(sid)
	return M.players[sid]
end

function M.is_human(sid)
	return M.humans[sid] == true
end

function M.human_list()
	local out = {}
	for sid in pairs(M.humans) do
		local pl = M.players[sid]
		if pl then out[#out + 1] = pl end
	end
	table.sort(out, function(a, b) return M.by_pid[a.id] < M.by_pid[b.id] end)
	return out
end

function M.weapon(name)
	if name == nil then return 0 end
	name = R.label(name)
	local wid = M.wids[name]
	if wid then return wid end
	wid = M.next_wid
	M.next_wid = wid + 1
	M.wids[name] = wid
	M.wnames[wid] = name
	R.row('dict', { kind = 'wpn', id = wid, name = name })
	return wid
end

-- material id: RayCastHit.material is DataContainer|nil (shared/type/raycasthit.md); after a nil check it is cast with
-- MaterialContainerPair(hit.material) (fb/materialcontainerpair.md, read only) -> {flagsAndIndex, physicsMaterialIndex,
-- physicsPropertyIndex}; mapping to our grid is decided after Phase 0 (DESIGN 3.4). Returns the triple and an id.
function M.material(dc)
	if dc == nil then return nil, 0 end
	local ok, mcp = pcall(function() return MaterialContainerPair(dc) end)
	if not ok or mcp == nil then return nil, 0 end
	local tri = { math.tointeger(mcp.flagsAndIndex) or 0, math.tointeger(mcp.physicsMaterialIndex) or 0,
		math.tointeger(mcp.physicsPropertyIndex) or 0 }
	local key = tri[1] .. ':' .. tri[2] .. ':' .. tri[3]
	local mid = M.mats[key]
	if not mid then
		mid = M.next_mat
		M.next_mat = mid + 1
		M.mats[key] = mid
		R.row('dict', { kind = 'mat', id = mid, name = key })
	end
	return tri, mid
end

-- flag(cp): CapturePointEntity -> small id; keyed by EntityBusPeer.instanceId (RO, shared/type/entitybuspeer.md).
function M.flag(cp)
	local key = cp.instanceId
	local id = M.cps[key]
	if id then
		M.cp_ent[id] = cp
		return id
	end
	id = M.next_cp
	M.next_cp = id + 1
	M.cps[key] = id
	M.cp_ent[id] = cp
	R.row('dict', { kind = 'cp', id = id, name = R.label(cp.name or ('cp' .. id)) })
	return id
end

function M.reset_level()
	M.cps = {}
	M.cp_ent = {}
	M.next_cp = 1
end

return M

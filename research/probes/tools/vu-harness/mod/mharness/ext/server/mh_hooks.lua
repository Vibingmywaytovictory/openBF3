-- Pass-through hooks (DESIGN.md section 3.10). Never Pass, never Return (content/vext/guides/hooks.md).
-- EntityFactory:Create (server/hook/entityfactory_create.md): Call() only for projectile data types -> 'shot'.
--   Data type names BulletEntityData, MissileEntityData, GrenadeEntityData (children of MeshProjectileEntityData) and
--   ExplosionEntityData (fb/*.md); compared on DataContainer.typeInfo.name (shared/type/datacontainer.md) [live:L12].
-- BulletEntity:Collision (server/hook/bulletentity_collision.md): read only -> 'hit' [live:L13].
-- Soldier:Damage (server/hook/soldier_damage.md): 'dmg' pre, Call(), 'dmg' post with soldier.health [live:L14].
local R = require('__shared/mh_row')
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local ids = require('mh_ids')
local bots = require('mh_bots')
local S = require('mh_samplers')

local M = {}

M.PROJ = { BulletEntityData = 'bullet', MissileEntityData = 'missile', GrenadeEntityData = 'grenade' }
M.dir_sign = 1          -- L12: the shot transform's forward sign (P0.8 may flip it)
M.recent_shots = {}     -- ring of {k, sid, uid, pos, dir}
M.recent_hits = {}      -- ring of {k, uid, pos, n, vic, shooter, body}
M.dmg_log = {}          -- ring of {k, vic, giver, pre, post, hp0, hp1, cli, dist}
M.uid_shooter = {}      -- uid -> sid
M.tracked = {}          -- uid -> {ent, sid, kind, last}
M.n_create = 0
M.n_proj = 0
M.ishit = {}            -- P0 L32 probe results
M.human_watch = {}      -- sid -> true: humans attributed for shots (watch me, U scenarios)

local RING = 400

local function push(ring, item)
	ring[#ring + 1] = item
	if #ring > RING then table.remove(ring, 1) end
end

function M.clear_rings()
	M.recent_shots = {}
	M.recent_hits = {}
	M.dmg_log = {}
end

local function shooter_near(pos)
	local best, bd = 0, 2.0
	for sid, b in pairs(bots.bots) do
		local e = bots.eye(b)
		if e then
			local d = V.dist(e, pos)
			if d < bd then best, bd = sid, d end
		end
	end
	for sid in pairs(M.human_watch) do
		local so = S.soldier_of(sid)
		if so ~= nil then
			local p = V.v(so.worldTransform.trans)
			local e = { p[1], p[2] + 1.5, p[3] }
			local d = V.dist(e, pos)
			if d < bd then best, bd = sid, d end
		end
	end
	return best
end

local function wid_of_sid(sid)
	local so = S.soldier_of(sid)
	if so == nil then return 0 end
	local ok, n = pcall(function() return so.weaponsComponent.currentWeapon.name end)
	if ok then return ids.weapon(n) end
	return 0
end

local function on_create(hookCtx, data, tr)
	M.n_create = M.n_create + 1
	if data == nil then return end
	local tn = data.typeInfo.name
	if tn == 'ExplosionEntityData' then
		local pos = V.v(tr.trans)
		R.row('ev', { name = 'explosion', pos = pos, id = shooter_near(pos) })
		return
	end
	local kind = M.PROJ[tn]
	if kind == nil then return end
	M.n_proj = M.n_proj + 1
	local ent = hookCtx:Call()
	local uid = 0
	if ent ~= nil then
		local ok, u = pcall(function() return ent.uniqueId end)
		if ok and u then uid = math.tointeger(u) or 0 end
	end
	local pos = V.v(tr.trans)
	local dir = V.scale(V.v(tr.forward), M.dir_sign)
	local sid = shooter_near(pos)
	if sid ~= 0 then
		local c = S.counter(sid)
		c.shot = c.shot + 1
		c.shot_seen = true
	end
	if uid ~= 0 then M.uid_shooter[uid] = sid end
	local okm, ism = pcall(function() return data:Is('MeshProjectileEntityData') end)
	M.ishit[tn] = okm and ism == true
	push(M.recent_shots, { k = C.gk, sid = sid, uid = uid, pos = pos, dir = dir })
	if kind ~= 'bullet' and ent ~= nil then M.tracked[uid] = { ent = ent, sid = sid, kind = kind, last = pos } end
	R.row('shot', { id = sid, wid = wid_of_sid(sid), kind = kind, uid = uid, pos = pos, dir = dir, td = tn })
end

local function victim_of(rb)
	if rb == nil then return nil, nil end
	local tn = nil
	pcall(function() tn = rb.typeInfo.name end)
	local vic = nil
	pcall(function()
		if rb:Is('SoldierEntity') then
			local so = SoldierEntity(rb)
			vic = ids.player(so.player)
		end
	end)
	return vic, tn
end

local function on_collision(hookCtx, entity, hit, giver)
	local uid = 0
	pcall(function() uid = math.tointeger(entity.uniqueId) or 0 end)
	local pos = V.v(hit.position)
	local n = V.v(hit.normal)
	local tri = ids.material(hit.material)
	local vic, body = victim_of(hit.rigidBody)
	local shooter = M.uid_shooter[uid]
	if shooter == nil and giver ~= nil then
		pcall(function() shooter = ids.player(giver.giver) end)
	end
	if shooter and shooter ~= 0 then
		local c = S.counter(shooter)
		c.hit = c.hit + 1
	end
	push(M.recent_hits, { k = C.gk, uid = uid, pos = pos, n = n, vic = vic, shooter = shooter, body = body })
	R.row('hit', { uid = uid, pos = pos, n = n, bone = math.tointeger(hit.bone) or -1,
		part = math.tointeger(hit.part) or -1, mat = tri, body = body, shooter = shooter, vic = vic })
end

local function on_damage(hookCtx, soldier, info, giver)
	local vic = ids.player(soldier.player)
	local hp0 = soldier.health
	local pos, org = V.v(info.position), V.v(info.origin)
	local gid, wid, spos, wfd, dtype = nil, nil, nil, nil, nil
	if giver ~= nil then
		pcall(function()
			local gp = giver.giver
			if gp ~= nil then
				gid = ids.player(gp)
				local gs = gp.soldier
				if gs ~= nil then
					spos = V.v(gs.worldTransform.trans)
					wid = ids.weapon(gs.weaponsComponent.currentWeapon.name)
				end
			end
		end)
		pcall(function() dtype = math.tointeger(giver.damageType) end)
		pcall(function()
			local d = giver.weaponFiring
			if d ~= nil then wfd = R.label(d.typeInfo.name) end
		end)
	end
	local dist = nil
	if pos and org then dist = V.dist(pos, org) end
	local fxd = nil
	local fp = bots.fxpos[vic or -1]
	if fp and spos then fxd = V.dist(fp, spos) end
	R.row('dmg', { victim = vic or 0, ph = 'pre', dmg = info.damage, hp = hp0, bone = math.tointeger(info.boneIndex),
		pos = pos, org = org, dir = V.v(info.direction), bul = info.isBulletDamage == true,
		exp = info.isExplosionDamage == true, cli = info.isClientDamage == true, cspd = info.collisionSpeed,
		dot = info.distributeDamageOverTime, giver = gid, wid = wid, dtype = dtype, dist = dist, fxd = fxd, spos = spos,
		wfd = wfd })
	local dmg = info.damage
	hookCtx:Call()
	local hp1 = soldier.health
	R.row('dmg', { victim = vic or 0, ph = 'post', dmg = dmg, hp = hp1 })
	push(M.dmg_log, { k = C.gk, vic = vic, giver = gid, pre = dmg, hp0 = hp0, hp1 = hp1, cli = info.isClientDamage == true,
		dist = dist })
end

-- Per-tick positions of tracked missiles/grenades (W-C detonation point = last position).
function M.on_postsim()
	for uid, t in pairs(M.tracked) do
		local ok, pos = pcall(function()
			local sp = SpatialEntity(t.ent)
			return V.v(sp.transform.trans)
		end)
		if ok and pos then
			t.last = pos
		else
			R.row('ev', { name = 'proj-end', id = t.sid, pos = t.last, a = { uid = uid } })
			M.tracked[uid] = nil
		end
	end
end

function M.install()
	Hooks:Install('EntityFactory:Create', 1, function(hookCtx, entityData, transform)
		R.safe('hook.create', on_create, hookCtx, entityData, transform)
	end)
	Hooks:Install('BulletEntity:Collision', 1, function(hookCtx, entity, hit, giverInfo)
		R.safe('hook.coll', on_collision, hookCtx, entity, hit, giverInfo)
	end)
	Hooks:Install('Soldier:Damage', 1, function(hookCtx, soldier, info, giverInfo)
		R.safe('hook.dmg', on_damage, hookCtx, soldier, info, giverInfo)
	end)
end

return M

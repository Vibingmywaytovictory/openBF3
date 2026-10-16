-- S-B movement (DESIGN.md 6.4). B. Items S3, W2 minimum rise, stance transition, T1.
-- Requires site flat (start P, direction u). Every input step is marked with its run-relative tick k0 so the motor
-- fit starts at the commanded step. Inputs: EIAThrottle 0, EIAStrafe 1, EIAJump 12, EIASprint 32
-- (fb/entryinputactionenum.md) [live:L08].
local V = require('__shared/mh_vec')
local C = require('__shared/mh_clock')
local bots = require('mh_bots')
local S = require('mh_samplers')
local L = require('scenarios/lib')

local M = { id = 'SB', title = 'S-B movement' }

local A = EntryInputActionEnum
M.DROPS = { 5, 8, 10, 12, 15, 20, 30, 50 }

function M.needs(args) return { sites = { 'flat' }, aimcal = true } end
function M.estimate(args) return math.floor(720 * math.max(0.25, tonumber(args.scale or '1') or 1)) end
function M.cost(args) return { s1 = 5, d1 = 4, r1 = 1 } end

local function step(ctx, label, rep, action, level, on_s, off_s, extra)
	ctx:mark('step', { cfg = label, i = rep, k0 = C.kk(), x = { a = action, l = level, on = on_s } })
	for _, a2 in ipairs(extra or {}) do bots.hold(ctx.bot, a2, 1.0) end
	bots.hold(ctx.bot, action, level)
	ctx:wait(on_s)
	bots.release(ctx.bot, action)
	for _, a2 in ipairs(extra or {}) do bots.release(ctx.bot, a2) end
	ctx:mark('step-off', { cfg = label, i = rep, k0 = C.kk() })
	ctx:wait(off_s)
end

local function home(ctx, flat, pose)
	local bot = ctx.bot
	if not bots.alive(bot) then
		if not L.respawn(ctx, bot, flat.pos, flat.dir, 0) then return false end
	end
	bots.place(bot, flat.pos, flat.dir)
	bots.face(bot, flat.dir)
	bots.pose(bot, pose or 0)
	ctx:wait(1.0)
	return true
end

function M.run(ctx, args)
	local flat = ctx:need_site('flat')
	if flat == nil then ctx:abort('no flat site') end
	local reps = L.scale(args, 3)
	local bot = L.spawn(ctx, 1, 'M16A3', flat.pos, flat.dir, 0)
	if bot == nil then ctx:abort('spawn failed') end
	ctx.bot = bot
	S.watch(bot.sid, { hp = true, gs = true, wf = true })
	local n_cfg = 7
	ctx:config(1, n_cfg, 'accel', nil, function()
		for _, pose in ipairs({ 0, 1, 2 }) do
			for r = 1, reps do
				if not home(ctx, flat, pose) then return false end
				step(ctx, 'accel-' .. pose, r, A.EIAThrottle, 1.0, 3.0, 3.0)
			end
		end
		return true
	end)
	ctx:config(2, n_cfg, 'sprint', nil, function()
		for r = 1, reps do
			if not home(ctx, flat, 0) then return false end
			step(ctx, 'sprint', r, A.EIAThrottle, 1.0, 4.0, 3.0, { A.EIASprint })
		end
		return true
	end)
	ctx:config(3, n_cfg, 'strafe-back', nil, function()
		for r = 1, reps do
			if not home(ctx, flat, 0) then return false end
			step(ctx, 'strafe', r, A.EIAStrafe, 1.0, 3.0, 3.0)
		end
		for r = 1, reps do
			if not home(ctx, flat, 0) then return false end
			step(ctx, 'back', r, A.EIAThrottle, -1.0, 3.0, 3.0)
		end
		return true
	end)
	ctx:config(4, n_cfg, 'jump', nil, function()
		if not home(ctx, flat, 0) then return false end
		for r = 1, L.scale(args, 5) do
			ctx:mark('jump', { cfg = 'jump', i = r, k0 = C.kk() })
			bots.press(bot, A.EIAJump, 1)
			ctx:wait(2.5)
		end
		return true
	end)
	ctx:config(5, n_cfg, 'drop', nil, function()
		local limit = (flat.up or 60) - 2
		for r, h in ipairs(M.DROPS) do
			if h <= limit and (args.scale == nil or r <= 2 + L.scale(args, 6)) then
				if not home(ctx, flat, 0) then return false end
				local p = { flat.pos[1], flat.pos[2] + h, flat.pos[3] }
				local hp0 = bots.health(bot)
				ctx:mark('drop', { cfg = 'drop', i = r, k0 = C.kk(), x = { h = h, hp0 = hp0 } })
				bots.place(bot, p)
				local landed_k = nil
				ctx:wait_until(function()
					if not bots.alive(bot) then return true end
					local q = bots.pos(bot)
					local so = bots.soldier(bot)
					local vy = 0
					pcall(function() vy = so.velocity.y end)
					if q and q[2] < flat.pos[2] + 0.3 and math.abs(vy) < 0.05 then
						landed_k = landed_k or C.kk()
						return true
					end
					return false
				end, 8, 'land')
				ctx:wait(2.0)
				ctx:mark('drop-end', { cfg = 'drop', i = r, k0 = C.kk(), x = { h = h, hp1 = bots.health(bot),
					dead = not bots.alive(bot) } })
			end
		end
		return true
	end)
	ctx:config(6, n_cfg, 'pose', nil, function()
		local seq = { 0, 1, 2, 1, 0, 2, 0 }
		for pass = 1, 2 do
			if not home(ctx, flat, 0) then return false end
			for i, pose in ipairs(seq) do
				ctx:mark('pose', { cfg = pass == 1 and 'pose-idle' or 'pose-fire', i = i, k0 = C.kk(), x = { pose = pose } })
				bots.pose(bot, pose)
				if pass == 1 then
					ctx:wait(3.0)
				else
					for _ = 1, 10 do
						L.fire1(ctx, bot)
						ctx:wait(0.3)
					end
				end
			end
		end
		return true
	end)
	ctx:config(7, n_cfg, 'minrise', nil, function()
		for _, zoom in ipairs({ false, true }) do
			for r = 1, reps do
				if not home(ctx, flat, 0) then return false end
				bots.zoom(bot, zoom)
				ctx:wait(2.0)
				step(ctx, zoom and 'minrise-ads' or 'minrise-hip', r, A.EIAThrottle, 1.0, 3.0, 3.0)
				bots.zoom(bot, false)
			end
		end
		return true
	end)
end

function M.cleanup(ctx) end

return M

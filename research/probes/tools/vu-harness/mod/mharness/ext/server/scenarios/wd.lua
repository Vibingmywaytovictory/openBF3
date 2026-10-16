-- W-D client (DESIGN.md 6.10). U. Items W8, W9, W10.
-- Starts client recording (mh:ctl rec={gs, fov}); server shot/wf rows for the human (L25); client gs pred/corr and
-- fov rows land in the VU client log. The user follows prompts and confirms each with !mh ok.
local ids = require('mh_ids')
local S = require('mh_samplers')
local H = require('mh_hooks')
local meta = require('mh_meta')

local M = { id = 'WD', title = 'W-D client' }

function M.needs(args) return {} end
function M.cost(args) return {} end
function M.estimate(args) return 900 end

local STEPS = {
	{ 'mags', 'fire 3 full-auto M16A3 magazines at the wall (reload when empty), then !mh ok' },
	{ 'reloads', 'reload at 0 %, 50 % and 90 % of a magazine, then !mh ok' },
	{ 'optics', 'aim down sights with every optic you have on the M16A3, hold each 3 s, then !mh ok' },
	{ 'scope', 'aim down the M98B scope and zoom levels for 3 s each, then !mh ok' },
}

function M.run(ctx, args)
	local sid = ids.player(ctx.owner)
	H.human_watch[sid] = true
	S.watch(sid, { wf = true, gs = true, hp = true })
	meta.client_ctl(ctx.owner, { gs = true, fov = true })
	for i, st in ipairs(STEPS) do
		ctx:config(i, #STEPS, st[1], nil, function()
			ctx.ok_flag = false
			ctx:prompt(st[1], { n = i, of = #STEPS, wait = 'ok' }, st[2])
			return ctx:wait_until(function() return ctx.ok_flag end, 600, st[1])
		end)
	end
	meta.client_ctl(ctx.owner, nil)
	H.human_watch[sid] = nil
	S.unwatch(sid)
end

function M.cleanup(ctx)
	H.human_watch = {}
end

return M

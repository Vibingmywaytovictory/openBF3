-- Clock and run ids (DESIGN.md section 3.3).
-- k: sim ticks since run start (UpdateManager:Update with UpdatePass_PostSim: shared/event/updatemanager_update.md,
-- fb/updatepass.md); t: sum of PostSim deltaTime; ms: SharedUtils:GetTimeMS() (shared/library/sharedutils.md).
-- Run id 'YYYYMMDD-<SC>-<HZ>-<HHMMSS>' from os.date at run start [live:L02]; SharedUtils:GetTickrate [live:L04].
-- Rows outside a scenario belong to the session pseudo-run 'YYYYMMDD-S-<HZ>-<HHMMSS>'.
-- The server entry advances k at UpdatePass_PreSim (hooks, GunSway and PostSim rows of one tick share k); if PreSim
-- never fires it uses PostSim, if neither fires Engine:Update [live:L03].
local M = {}

M.gk = 0          -- ticks since mod load
M.gt = 0.0        -- seconds since mod load (sum of dt)
M.k0 = 0          -- gk at the start of the current run
M.t0 = 0.0
M.cur_run = nil
M.cur_sc = 'S'
M.session = nil
M.hzv = 30
M.mode = 'presim'   -- 'presim' (k advances at PreSim), 'postsim' or 'engine' fallbacks [live:L03]
M.last_dt = 1 / 30
M.used = {}

local function tickrate()
	local ok, r = pcall(function() return SharedUtils:GetTickrate() end)
	if ok and type(r) == 'number' and r > 0 then return r end
	return 30
end

function M.hz()
	local r = tickrate()
	local best, bd = 30, math.huge
	for _, h in ipairs({ 30, 60, 120 }) do
		if math.abs(h - r) < bd then best, bd = h, math.abs(h - r) end
	end
	M.hzv = best
	return best
end

local function stamp()
	local ok, d, h = pcall(function() return os.date('%Y%m%d'), os.date('%H%M%S') end)
	if ok and type(d) == 'string' and #d == 8 and type(h) == 'string' and #h == 6 then return d, h end
	-- Fallback when os.date is unavailable [live:L02]: day 00000000 and the ms clock as a time of day.
	local ms = 0
	pcall(function() ms = SharedUtils:GetTimeMS() end)
	local s = math.floor(ms / 1000) % 86400
	return '00000000', string.format('%02d%02d%02d', s // 3600, (s // 60) % 60, s % 60)
end

local function make_id(sc)
	local d, h = stamp()
	local id = string.format('%s-%s-%d-%s', d, sc, M.hz(), h)
	local bump = 0
	while M.used[id] and bump < 3600 do
		bump = bump + 1
		local hh, mm, ss = tonumber(h:sub(1, 2)), tonumber(h:sub(3, 4)), tonumber(h:sub(5, 6)) + bump
		mm = mm + ss // 60
		ss = ss % 60
		hh = (hh + mm // 60) % 24
		mm = mm % 60
		id = string.format('%s-%s-%d-%02d%02d%02d', d, sc, M.hzv, hh, mm, ss)
	end
	M.used[id] = true
	return id
end

function M.ensure_session()
	if not M.session then M.session = make_id('S') end
	return M.session
end

function M.on_tick(dt)
	if type(dt) ~= 'number' or dt <= 0 or dt ~= dt then dt = M.last_dt end
	M.last_dt = dt
	M.gk = M.gk + 1
	M.gt = M.gt + dt
end

function M.start_run(sc)
	M.cur_run = make_id(sc)
	M.cur_sc = sc
	M.k0 = M.gk
	M.t0 = M.gt
	return M.cur_run
end

-- adopt(run, sc): the client follows the server's run id (mh:ctl).
function M.adopt(run, sc)
	M.cur_run = run
	M.cur_sc = sc or 'S'
	M.k0 = M.gk
	M.t0 = M.gt
end

function M.end_run()
	M.cur_run = nil
	M.cur_sc = 'S'
end

function M.run_id()
	return M.cur_run or M.ensure_session()
end

function M.scen()
	return M.cur_run and M.cur_sc or 'S'
end

function M.kk()
	if M.cur_run then return M.gk - M.k0 end
	return M.gk
end

function M.tt()
	if M.cur_run then return M.gt - M.t0 end
	return M.gt
end

function M.dt()
	return M.last_dt
end

return M

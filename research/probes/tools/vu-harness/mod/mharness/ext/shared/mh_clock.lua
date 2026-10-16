-- Clock and run ids (SKELETON; DESIGN.md section 3.3).
-- k: sim ticks since run start (UpdateManager:Update with UpdatePass_PostSim: shared/event/updatemanager_update.md,
-- fb/updatepass.md); t: sum of PostSim deltaTime; ms: SharedUtils:GetTimeMS() (shared/library/sharedutils.md).
-- Run id 'YYYYMMDD-<SC>-<HZ>-<HHMMSS>' from os.date at run start [live:L02].
local M = {}

function M.on_postsim(deltaTime)
end

function M.start_run(sc)
	return nil
end

return M

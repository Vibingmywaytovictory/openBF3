-- GunSway -> 'gs' payload (SKELETON; DESIGN.md sections 3.10, 4). One field list for server and client.
-- GunSway fields: shared/type/gunsway.md; GunSwayDeviation: shared/type/gunswaydeviation.md. Units raw until L11.
local M = {}

-- payload(gunSway, id, wid, src, pose, zoom, nup, dt) -> table with da, dmin, rec, disp, ra, rr, seed, ifs, fs, ...
function M.payload(gunSway, id, wid, src, pose, zoom, nup, dt)
	return nil
end

return M

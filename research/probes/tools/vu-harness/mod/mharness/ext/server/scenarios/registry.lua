-- Scenario registry (DESIGN.md section 6). Session 2-3 entries exist disabled (extension points: V-A..V-C vehicles,
-- A-A..A-C air, M-C, N-A naval; they would add a module here, mh_bots.seat and the reserved veh/vdmg rows).
-- Literal requires (apicheck rule 10); no scenario module requires mh_runner at load time (no require cycle).
local M = {
	P0 = { mod = require('scenarios/p0'), who = 'U', enabled = true },
	WA = { mod = require('scenarios/wa'), who = 'B', enabled = true },
	SB = { mod = require('scenarios/sb'), who = 'B', enabled = true },
	WB = { mod = require('scenarios/wb'), who = 'U/B', enabled = true },
	SA = { mod = require('scenarios/sa'), who = 'U/B', enabled = true },
	MA = { mod = require('scenarios/ma'), who = 'B', enabled = true },
	MB = { mod = require('scenarios/mb'), who = 'B', enabled = true },
	WC = { mod = require('scenarios/wc'), who = 'U', enabled = true },
	WD = { mod = require('scenarios/wd'), who = 'U', enabled = true },
	VA = { enabled = false }, VB = { enabled = false }, VC = { enabled = false },
	AA = { enabled = false }, AB = { enabled = false }, AC = { enabled = false },
	MC = { enabled = false }, NA = { enabled = false },
}

return M

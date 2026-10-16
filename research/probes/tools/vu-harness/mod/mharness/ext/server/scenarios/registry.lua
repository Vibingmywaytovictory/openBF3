-- Scenario registry (SKELETON; DESIGN.md section 6). Session 2-3 entries exist disabled (extension points).
local M = {
	P0 = { module = 'scenarios/p0', who = 'U', enabled = true },
	WA = { module = 'scenarios/wa', who = 'B', enabled = true },
	SB = { module = 'scenarios/sb', who = 'B', enabled = true },
	WB = { module = 'scenarios/wb', who = 'U/B', enabled = true },
	SA = { module = 'scenarios/sa', who = 'U/B', enabled = true },
	MA = { module = 'scenarios/ma', who = 'B', enabled = true },
	MB = { module = 'scenarios/mb', who = 'B', enabled = true },
	WC = { module = 'scenarios/wc', who = 'U', enabled = true },
	WD = { module = 'scenarios/wd', who = 'U', enabled = true },
	VA = { enabled = false }, VB = { enabled = false }, VC = { enabled = false },
	AA = { enabled = false }, AB = { enabled = false }, AC = { enabled = false },
	MC = { enabled = false }, NA = { enabled = false },
}

return M

-- W-A configurations in queue order (SKELETON; DESIGN.md section 6.2). Unlock names are resolved by P0.2 [live:L07].
local M = {}

M.protocols = {
	full = { a = 200, b = 100, c = 20, d = 10, e = 15, gaps = { 0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5 } },
	short = { a = 100, b = 50, c = 10, d = 5, e = 10, gaps = { 0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5 } },
	bolt = { a = 200, c = 20, gaps = { 1.5, 2.0, 3.0 } },
}

M.distance = { hip = 10, ads = 25, ads_mag = 12 }

M.configs = {
	{ tier = 1, w = 'M16A3', stance = 'stand', zoom = true, proto = 'full' },
	{ tier = 1, w = 'M16A3', stance = 'stand', zoom = false, proto = 'full' },
	{ tier = 2, w = 'M16A3', stance = 'prone', zoom = true, proto = 'full' },
	{ tier = 2, w = 'M16A3', stance = 'crouch', zoom = true, proto = 'full' },
	{ tier = 2, w = 'M16A3', stance = 'crouch', zoom = false, proto = 'full' },
	{ tier = 2, w = 'M16A3', stance = 'prone', zoom = false, proto = 'full' },
	{ tier = 3, w = 'AK74M', stance = 'stand', zoom = true, proto = 'short' },
	{ tier = 3, w = 'AK74M', stance = 'stand', zoom = false, proto = 'short' },
	{ tier = 3, w = 'AK74M', stance = 'prone', zoom = true, proto = 'short' },
	{ tier = 3, w = 'M416', stance = 'stand', zoom = true, proto = 'short' },
	{ tier = 3, w = 'M416', stance = 'stand', zoom = false, proto = 'short' },
	{ tier = 3, w = 'M416', stance = 'prone', zoom = true, proto = 'short' },
	{ tier = 3, w = 'M249', stance = 'stand', zoom = true, proto = 'short' },
	{ tier = 3, w = 'M249', stance = 'stand', zoom = false, proto = 'short' },
	{ tier = 3, w = 'M249', stance = 'prone', zoom = true, proto = 'short' },
	{ tier = 3, w = 'M98B', stance = 'stand', zoom = true, proto = 'bolt' },
	{ tier = 3, w = 'M98B', stance = 'prone', zoom = true, proto = 'bolt' },
}

M.tier60 = {
	{ w = 'M16A3', stance = 'stand', zoom = false, proto = 'short' },
	{ w = 'M16A3', stance = 'stand', zoom = true, proto = 'short' },
}

-- Candidate unlock asset names (the first that resolves wins). Source: research/PORTING-RESEARCH.md section 7.1 and
-- our own dumps; the M416 path is inferred.
M.unlocks = {
	M16A3 = { 'Weapons/M16A4/U_M16A4', 'weapons/m16a4/u_m16a4' },
	AK74M = { 'Weapons/AK74M/U_AK74M', 'weapons/ak74m/u_ak74m' },
	M416 = { 'Weapons/M416/U_M416', 'weapons/m416/u_m416' },
	M249 = { 'Weapons/M249/U_M249', 'weapons/m249/u_m249' },
	M98B = { 'Weapons/Model98B/U_M98B', 'weapons/model98b/u_m98b' },
	M9 = { 'Weapons/M9/U_M9', 'weapons/m9/u_m9' },
	['870MCS'] = { 'Weapons/Remington870/U_870', 'weapons/remington870/u_870' },
	RPG7 = { 'Weapons/RPG7/U_RPG7', 'weapons/rpg7/u_rpg7' },
	SMAW = { 'Weapons/SMAW/U_SMAW', 'weapons/smaw/u_smaw' },
	M320HE = { 'Weapons/Gadgets/M320/U_M320_HE', 'weapons/gadgets/m320/u_m320_he' },
	M67 = { 'Weapons/M67/U_M67', 'weapons/m67/u_m67' },
}
M.soldier_blueprint = { 'Characters/Soldiers/MpSoldier', 'characters/soldiers/mpsoldier' }
M.kits = {
	team1 = { 'Gameplay/Kits/USAssault', 'gameplay/kits/usassault' },
	team2 = { 'Gameplay/Kits/RUAssault', 'gameplay/kits/ruassault' },
}

return M

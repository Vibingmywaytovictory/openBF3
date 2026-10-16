-- W-A configurations in queue order (DESIGN.md section 6.2) and the weapon tables every scenario uses.
-- Unlock names are resolved by P0.2 [live:L07].
local M = {}

M.protocols = {
	full = { a = 200, b = 100, c = 20, d = 10, e = 15, gaps = { 0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5 } },
	short = { a = 100, b = 50, c = 10, d = 5, e = 10, gaps = { 0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5 } },
	bolt = { a = 200, c = 20, gaps = { 1.5, 2.0, 3.0 } },
}

M.distance = { hip = 10, ads = 25, ads_mag = 12 }

-- Team per weapon so the shooter's own kit lists it (topics/01 section 1 kit table: M16A3 US, AK-74M RU, M416 both)
-- [inferred; live:L29]. TeamId.Team1 = US, Team2 = RU on the Session 1 levels [inferred; P0.6 logs it].
M.team = { M16A3 = 1, M416 = 1, M249 = 1, M98B = 1, M9 = 1, ['870MCS'] = 1, SMAW = 1, M320HE = 1, M67 = 1,
	AK74M = 2, RPG7 = 2 }

-- Weapon slot per weapon (fb/weaponslot.md values; which slot each weapon class uses is [inferred; live:L31]:
-- 0 primary, 1 secondary, 2 and 5 gadgets, 6 grenade). Verified per spawn with weaponsComponent.currentWeaponSlot.
M.slot = { M16A3 = 0, AK74M = 0, M416 = 0, M249 = 0, M98B = 0, ['870MCS'] = 0, M9 = 1, RPG7 = 2, SMAW = 2,
	M320HE = 2, M67 = 6 }

-- Fire mode each weapon's protocols need (FireLogicType, fb/firelogictype.md: 2 auto, 1 bolt action). The bot cycles
-- with EIACycleFireMode until SoldierWeapon.fireLogic matches (DESIGN.md 3.9) [live:L30].
M.fire = { M16A3 = 2, AK74M = 2, M416 = 2, M249 = 2, M98B = 1 }

-- Case-insensitive aliases accepted in chat arguments (DESIGN.md 3.7).
M.alias = { ['870'] = '870MCS', ['AK-74M'] = 'AK74M', M16 = 'M16A3', ['M98'] = 'M98B', RPG = 'RPG7', M320 = 'M320HE' }

-- Queue presets: WA:tier1 = configs 1-2, WA:rest = 3-17, WA:all = 1-17 (DESIGN.md 6.2).
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
	{ tier = 3, w = 'M98B', stance = 'stand', zoom = true, proto = 'bolt', sway = true },
	{ tier = 3, w = 'M98B', stance = 'prone', zoom = true, proto = 'bolt', sway = true },
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

-- Rounds per minute (P section 7.1, data/weapon_constants.json); FIRE_N timeouts and estimates only.
M.rpm = { M16A3 = 800, AK74M = 650, M416 = 750, M249 = 800, M98B = 46, M9 = 400, ['870MCS'] = 70 }

-- Rounds per life (magazine + reserve) [inferred]: only for the ticket forecast of W-A respawns.
M.ammo_total = { M16A3 = 151, AK74M = 151, M416 = 151, M249 = 300, M98B = 30 }

-- Soldier appearance assets for bots (SelectUnlockAssets) [inferred names; resolved by P0.2, skipped if absent].
M.appearance = {
	team1 = { 'Persistence/Unlocks/Soldiers/Visual/MP/Us/MP_US_Assault_Appearance01',
		'persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01' },
	team2 = { 'Persistence/Unlocks/Soldiers/Visual/MP/RU/MP_RU_Assault_Appearance01',
		'persistence/unlocks/soldiers/visual/mp/ru/mp_ru_assault_appearance01' },
}

-- Damage per weapon for the heal policy (falloff knots; P section 7.1) and the zone multiplier upper bound.
M.falloff = {
	M16A3 = { 25, 18.4, 8, 50 }, AK74M = { 25, 18.4, 8, 50 }, M416 = { 25, 18.4, 8, 50 },
	M98B = { 95, 59, 15, 150 }, M9 = { 20, 12.5, 8, 50 }, ['870MCS'] = { 100, 25, 5, 30 },
}
M.zone_mult = { chest = 1.0, head = 2.5 }

function M.expected_damage(w, d, zone)
	local f = M.falloff[w]
	if f == nil then return 100 end
	local dmg
	if d <= f[3] then dmg = f[1] elseif d >= f[4] then dmg = f[2] else
		dmg = f[1] + (f[2] - f[1]) * (d - f[3]) / (f[4] - f[3])
	end
	return dmg * (M.zone_mult[zone] or 1.0)
end

-- canonical(name): accept keys and aliases case-insensitively (DESIGN.md 3.7).
function M.canonical(name)
	if name == nil then return nil end
	local up = name:upper()
	for key in pairs(M.unlocks) do
		if key:upper() == up then return key end
	end
	for a, key in pairs(M.alias) do
		if a:upper() == up then return key end
	end
	return nil
end

return M

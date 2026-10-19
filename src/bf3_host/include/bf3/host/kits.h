#pragma once
// The match's kits (docs/design/m2a.md §5.6; owner sv): the default TDM loadouts of the two teams, resolved to weapon
// def ids of the match's WeaponTable. M2a resolves only slots 0 (primary), 1 (sidearm), 6 (grenade) and 7 (knife),
// "1 primary + pistol + knife + M67" (plan); the gadgets of slots 2, 4 and 5 are reported as skipped (§16 Q16).
// Metro TDM [verified: bf3dump kits MP_Subway TeamDeathMatch0]: assault u_m16a4 / u_ak74m, engineer u_m4a1 /
// u_aks74u, support u_m27iar / u_rpk-74m, recon u_mk11 / u_svd; sidearm US u_m9, RU u_mp443; u_m67; u_knife.
// 12 distinct items. Bots take kit slot % 4 and S1's alternating teams 1 / 2; the local player picks with KitSelect.

#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view> // M2b ru
#include <vector>

namespace bf3::host {

inline constexpr uint8_t kKitCount = 4; // assault, engineer, support, recon
inline constexpr uint8_t kKitTeams = 2; // teams 1 and 2 (index team - 1)
inline constexpr std::array<uint8_t, 4> kKitSlots{ 0, 1, 6, 7 }; // the WeaponSlots M2a resolves
inline constexpr uint8_t kNoTeamChange = 0; // KitSelect team 0: keep the current team
inline constexpr uint8_t kKitGadgetSlot = 2; // M2b W1.0 (ru): the assault gadget slot (the defibrillator with TDM kits, #9)

enum class KitRole : uint8_t { Assault = 0, Engineer = 1, Support = 2, Recon = 3 };
const char *kit_role_name(KitRole p_role);

struct KitLoadout {
	uint8_t kit = 0; // KitRole
	uint8_t team = 1; // 1 US, 2 RU
	std::array<uint16_t, sim::weapons::kMaxWeaponSlots> defs{}; // def id per slot, sim::weapons::kNoWeapon when empty
	std::array<std::string, sim::weapons::kMaxWeaponSlots> unlocks; // the unlock partition per filled slot (reports)
	uint8_t active = 0; // the slot in hand at spawn (the primary)
};

struct KitTable {
	std::array<std::array<KitLoadout, kKitCount>, kKitTeams> loadouts{}; // [team - 1][kit]
	std::vector<std::string> skipped; // "<kit>/<slot>: <unlock>" for the slots M2a leaves empty
	uint64_t hash = 0; // kit_table_hash: MatchInfo::kit_hash
	// M2b W1.0 (ru, #9): the def id of KitTableOptions::assault_gadget in assault slot kKitGadgetSlot (both teams),
	// sim::weapons::kNoWeapon without the option. Hashed only when set, so an M2a table's hash is unchanged.
	uint16_t defib_def = sim::weapons::kNoWeapon;

	const KitLoadout &loadout(uint8_t p_team, uint8_t p_kit) const; // team 1..2, kit < kKitCount (clamped)
};

// The kits of the mode's two teams (TeamDef::kits in order, KitDef::default_unlock per slot), each default unlock
// matched to a def of p_defs by WeaponDef::variant (the index into p_defs is the table's def id). Fails when a team
// does not list kKitCount kits, or a resolved slot's unlock has no def.
core::Result<KitTable> make_kit_table(std::span<const core::gameplay::TeamDef> p_teams, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs);
// M2b W1.0 (docs/design/m2b.md §2.6 #9, §3.6, J1; owner ru): with a non-empty assault_gadget (the defibrillator
// variant, weapons/gadgets/defibrillator/u_defib), the assault kit of both teams gets that def in slot kKitGadgetSlot
// and KitTable::defib_def is set. Empty = M2a: the 3-argument make_kit_table calls this one with empty options and its
// table, hash and callers are unchanged. kKitSlots is not changed.
struct KitTableOptions {
	std::string assault_gadget;
};
// M2b ru (as built): the TDM kits' assault gadget [verified: bf3dump weapon defibrillator] (the variant, lowercase).
inline constexpr std::string_view kTdmAssaultGadget = "weapons/gadgets/defibrillator/u_defib";
core::Result<KitTable> make_kit_table(std::span<const core::gameplay::TeamDef> p_teams, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs, const KitTableOptions &p_options);
// FNV-1a 64 over every loadout (team, kit, def ids, active) in order: the client refuses another server's kits.
uint64_t kit_table_hash(const KitTable &p_table);

} // namespace bf3::host

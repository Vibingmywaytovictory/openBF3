#pragma once

#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::host {

inline constexpr uint8_t kKitCount = 4;
inline constexpr uint8_t kKitTeams = 2;
inline constexpr std::array<uint8_t, 4> kKitSlots{ 0, 1, 6, 7 };
inline constexpr uint8_t kNoTeamChange = 0;
inline constexpr uint8_t kKitGadgetSlot = 2;

enum class KitRole : uint8_t { Assault = 0, Engineer = 1, Support = 2, Recon = 3 };
const char *kit_role_name(KitRole p_role);

struct KitLoadout {
	uint8_t kit = 0;
	uint8_t team = 1;
	std::array<uint16_t, sim::weapons::kMaxWeaponSlots> defs{};
	std::array<std::string, sim::weapons::kMaxWeaponSlots> unlocks;
	uint8_t active = 0;
	int32_t hud_icon = 0;
	std::string asset;
};

struct KitTable {
	std::array<std::array<KitLoadout, kKitCount>, kKitTeams> loadouts{};
	std::vector<std::string> skipped;
	uint64_t hash = 0;

	uint16_t defib_def = sim::weapons::kNoWeapon;

	const KitLoadout &loadout(uint8_t p_team, uint8_t p_kit) const;
};

core::Result<KitTable> make_kit_table(std::span<const core::gameplay::TeamDef> p_teams, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs);

struct KitTableOptions {
	std::string assault_gadget;
};

inline constexpr std::string_view kTdmAssaultGadget = "weapons/gadgets/defibrillator/u_defib";
core::Result<KitTable> make_kit_table(std::span<const core::gameplay::TeamDef> p_teams, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs, const KitTableOptions &p_options);

uint64_t kit_table_hash(const KitTable &p_table);

}

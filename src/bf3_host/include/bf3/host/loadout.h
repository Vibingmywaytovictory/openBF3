#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"
#include "bf3/host/kits.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::host {

inline constexpr size_t kLoadoutSlots = core::gameplay::kWeaponSlotCount;
inline constexpr size_t kLoadoutAccessories = 4;
inline constexpr size_t kLoadoutAccessoryWeapon = size_t(core::gameplay::WeaponSlot::Primary);
static_assert(kLoadoutSlots == sim::weapons::kMaxWeaponSlots);

struct LoadoutItem {
	uint32_t identifier = 0;
	std::string unlock;
	uint16_t def = sim::weapons::kNoWeapon;
	std::array<std::vector<uint32_t>, kLoadoutAccessories> accessories;
};

struct LoadoutKit {
	std::array<std::vector<LoadoutItem>, kLoadoutSlots> slots;
	std::array<uint32_t, kLoadoutSlots> defaults{};
	std::vector<uint32_t> specializations;
	uint32_t label_sid_hash = 0;

	const LoadoutItem *find(size_t p_slot, uint32_t p_identifier) const;
};

struct LoadoutCatalog {
	std::array<std::array<LoadoutKit, kKitCount>, kKitTeams> kits;
	size_t items = 0;
	uint64_t hash = 0;

	bool empty() const { return items == 0; }
	const LoadoutKit *kit(uint8_t p_team, uint8_t p_kit) const;
};

struct LoadoutChoice {
	std::array<uint32_t, kLoadoutSlots> items{};
	std::array<uint32_t, kLoadoutAccessories> accessories{};
	uint32_t specialization = 0;

	bool operator==(const LoadoutChoice &) const = default;
};

struct Loadout {
	std::array<uint16_t, sim::weapons::kMaxWeaponSlots> defs{};
	LoadoutChoice choice;
	bool chosen = false;
};

std::vector<std::string> loadout_variants(std::span<const core::gameplay::KitDef> p_kits);

core::Result<LoadoutCatalog> make_loadout_catalog(const core::EbxResolver &p_db,
		const core::gameplay::UnlockProgression &p_progression, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs, const KitTable &p_table,
		core::gameplay::WorkBudget *p_budget = nullptr);

Loadout default_loadout(const LoadoutCatalog &p_catalog, const KitTable &p_table, uint8_t p_team, uint8_t p_kit);

core::Result<Loadout> resolve_loadout(const LoadoutCatalog &p_catalog, const KitTable &p_table, uint8_t p_team,
		uint8_t p_kit, const LoadoutChoice &p_choice);

uint64_t loadout_catalog_hash(const LoadoutCatalog &p_catalog);

}

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
	std::array<std::vector<std::string>, kLoadoutAccessories> accessory_unlocks;
	std::array<uint32_t, kLoadoutAccessories> default_accessories{};
};

struct LoadoutKit {
	std::array<std::vector<LoadoutItem>, kLoadoutSlots> slots;
	std::array<uint32_t, kLoadoutSlots> defaults{};
	std::array<uint32_t, kLoadoutSlots> category_sid_hashes{};
	std::vector<uint32_t> specializations;
	uint32_t default_specialization = 0;
	uint32_t specialization_sid_hash = 0;
	uint32_t label_sid_hash = 0;

	const LoadoutItem *find(size_t p_slot, uint32_t p_identifier) const;
};

inline constexpr size_t kLoadoutKitItems = 5;
inline constexpr uint8_t kLoadoutSpecializationSlot = uint8_t(kLoadoutSlots);

enum class LoadoutKitItem : uint8_t { Primary, Sidearm, Gadget1, Gadget2, Specialization };

inline constexpr std::array<std::array<uint8_t, 2>, kLoadoutKitItems> kLoadoutItemCategorySlots{ { { 0, 0 }, { 1, 1 },
		{ 2, 4 }, { 5, 2 }, { kLoadoutSpecializationSlot, kLoadoutSpecializationSlot } } };

struct LoadoutOption {
	uint8_t slot = 0;
	uint32_t identifier = 0;

	bool operator==(const LoadoutOption &) const = default;
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

struct LoadoutWeapon {
	uint16_t def = sim::weapons::kNoWeapon;
	std::string unlock;
	uint8_t slot = 0;
	uint8_t kits = 0;
};

std::vector<LoadoutWeapon> loadout_weapons(const LoadoutCatalog &p_catalog, uint8_t p_team);

const LoadoutItem *loadout_primary_item(const LoadoutCatalog &p_catalog, uint8_t p_team, uint16_t p_def);

struct LoadoutAccessorySet {
	std::vector<std::string> active;
	std::vector<std::string> listed;
	std::array<uint32_t, kLoadoutAccessories> picked{};

	bool operator==(const LoadoutAccessorySet &) const = default;
};

LoadoutAccessorySet loadout_accessory_set(
		const LoadoutItem &p_item, const std::array<uint32_t, kLoadoutAccessories> &p_accessories);

std::vector<uint8_t> loadout_item_slots(const LoadoutKit &p_kit, LoadoutKitItem p_item);

std::vector<LoadoutOption> loadout_item_options(const LoadoutKit &p_kit, LoadoutKitItem p_item);

LoadoutOption loadout_item_current(const LoadoutKit &p_kit, const LoadoutChoice &p_choice, LoadoutKitItem p_item);

uint32_t loadout_item_category_sid(const LoadoutKit &p_kit, LoadoutKitItem p_item);

bool set_loadout_item(const LoadoutKit &p_kit, LoadoutKitItem p_item, uint32_t p_identifier, LoadoutChoice &r_choice);

}

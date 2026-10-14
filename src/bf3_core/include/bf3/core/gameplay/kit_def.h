#pragma once
// KitDef and TeamDef: the four kits of a team and what each slot offers (plan §3.7; research/topics/
// 02-soldier-kits-modes.md §2.1-§2.3; probes/ebx §5).
//
// A kit is a VeniceSoldierCustomizationAsset with three CustomizationTables (WeaponTable,
// SpecializationTable, VisualTable) of CustomizationUnlockParts {UICategorySid, SelectableUnlocks[]}.
// The WeaponTable's UnlockParts index is the WeaponSlot: 0 primary, 1 sidearm, 2 gadget 1
// (launchers, recon tools), 3 unused, 4 kit item (medkit, ammo box), 5 gadget 2, 6 grenade, 7 knife.
// Weapons are named by partition only (the unlock asset and the SoldierWeaponBlueprint it unlocks);
// WeaponDefs are resolved elsewhere. "Default" is the first selectable entry of a slot: BF3 has no
// default loadout in data (the menu offers every SelectableUnlocks entry; progression is not
// reimplemented, plan §3.7).

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

enum class WeaponSlot : uint8_t { Primary = 0, Sidearm = 1, Gadget1 = 2, Unused3 = 3, KitItem = 4, Gadget2 = 5, Grenade = 6, Knife = 7 };
inline constexpr size_t kWeaponSlotCount = 8;
const char *weapon_slot_name(WeaponSlot p_slot);

struct KitUnlock {
	std::string unlock; // unlock asset partition, e.g. weapons/m16a4/u_m16a4
	std::string type; // SoldierWeaponUnlockAsset, UnlockAsset, ValueUnlockAsset
	std::string weapon; // SoldierWeaponUnlockAsset.Weapon partition (SoldierWeaponBlueprint), else empty
};

struct KitSlot {
	std::string ui_category_sid;
	std::vector<KitUnlock> unlocks;
};

struct KitDef {
	std::string asset; // partition, e.g. gameplay/kits/usassault
	std::string label_sid; // ID_M_ASSAULT
	std::string hud_icon; // UIHudIcon_KitAssault
	std::vector<KitSlot> weapon_slots; // by WeaponSlot index (as many as the table has)
	std::vector<KitUnlock> specializations; // SpecializationTable, all parts
	size_t visual_unlocks = 0; // VisualTable entries (camos), counted only

	const KitSlot *slot(WeaponSlot p_slot) const;
	// The first selectable unlock of a slot, nullptr when the slot is empty.
	const KitUnlock *default_unlock(WeaponSlot p_slot) const;
};

struct TeamDef {
	int team = 0; // 1, 2 (3, 4 in squad deathmatch); 0 neutral
	std::string team_data; // gameplay/teams/us
	std::string faction; // FactionUS, FactionRUS
	std::string soldier; // SoldierBlueprint partition
	std::vector<std::string> kits; // SoldierCustomization partitions in order
	std::vector<std::string> vehicle_customizations;
};

Result<KitDef> read_kit_def(const EbxResolver &p_db, std::string_view p_asset);
// A TeamData asset (gameplay/teams/us).
Result<TeamDef> read_team_data(const EbxResolver &p_db, const EbxInstance &p_team_data);

} // namespace bf3::core::gameplay

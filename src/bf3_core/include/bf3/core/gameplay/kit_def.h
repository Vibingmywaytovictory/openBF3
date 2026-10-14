#pragma once
// KitDef and TeamDef: the four kits of a team and what each slot offers (plan §3.7; research/topics/
// 02-soldier-kits-modes.md §2.1-§2.3; probes/ebx §5).
//
// A kit is a VeniceSoldierCustomizationAsset with three CustomizationTables (WeaponTable,
// SpecializationTable, VisualTable) of CustomizationUnlockParts {UICategorySid, SelectableUnlocks[]}.
// The WeaponTable's UnlockParts index is the WeaponSlot: 0 primary, 1 sidearm, 2 gadget 1
// (launchers, recon tools), 3 unused, 4 kit item (medkit, ammo box), 5 gadget 2, 6 grenade, 7 knife.
// Weapons are named by partition only (the unlock asset and the SoldierWeaponBlueprint it unlocks);
// WeaponDefs are resolved elsewhere.
//
// The kit tables carry no "default" flag and every unlock asset has UnlockScore 0 / AutoAvailable
// false, so the starting loadout comes from the persistence progression tables instead: an unlock is
// gated when one of them grants it (persistence/ranks/rankparams RankLevelData.UnlockInfos,
// persistence/unlocks/mpstaticunlocks UnlockInfos (licence unlocks), persistence/awards/awards
// AwardData.UnlockInfos, persistence/scoring/scoring ScoringBucketData.Unlocks[].UnlockInfo; each
// BasicUnlockInfo names the unlock asset's instance GUID). A slot's default is its first unlock that
// no table gates and that is not a variant (Extra set: an alternate-ammo unlock of a weapon, e.g.
// u_m320_shg, whose weapon-kill progression is not in these tables). The game itself offers every
// entry (no progression, plan §3.7); the default is what a fresh soldier starts with: m16a4 / m9 /
// medkit / m67 / knife for US assault (the razor knife listed first is a licence unlock). Without
// the tables the default is unknown (nullptr).

#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::gameplay {

enum class WeaponSlot : uint8_t { Primary = 0, Sidearm = 1, Gadget1 = 2, Unused3 = 3, KitItem = 4, Gadget2 = 5, Grenade = 6, Knife = 7 };
inline constexpr size_t kWeaponSlotCount = 8;
const char *weapon_slot_name(WeaponSlot p_slot);

// Which persistence table grants an unlock (the first one found, in the order below).
enum class UnlockGate : uint8_t { Free, Rank, Licence, Award, ScoringBucket };
const char *unlock_gate_name(UnlockGate p_gate);

// The unlock instance GUIDs the persistence progression tables grant.
struct UnlockProgression {
	std::unordered_map<Guid, UnlockGate, GuidHash> gates;
	size_t tables = 0; // progression partitions found (4 in an MP level superbundle)

	bool known() const { return tables != 0; }
	UnlockGate gate(const Guid &p_unlock) const;
};

// Reads the four progression tables named above; a missing table is skipped (tables counts them).
UnlockProgression read_unlock_progression(const EbxResolver &p_db);

struct KitUnlock {
	std::string unlock; // unlock asset partition, e.g. weapons/m16a4/u_m16a4
	std::string type; // SoldierWeaponUnlockAsset, UnlockAsset, ValueUnlockAsset
	std::string weapon; // SoldierWeaponUnlockAsset.Weapon partition (SoldierWeaponBlueprint), else empty
	UnlockGate gate = UnlockGate::Free;
	bool variant = false; // Extra set: an ammo/variant unlock of a weapon (u_m320_shg, u_m26mass_slug)
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
	bool progression_known = false; // the progression tables were found (gates are meaningful)

	const KitSlot *slot(WeaponSlot p_slot) const;
	// The first ungated, non-variant unlock of a slot (see the header comment); nullptr when the slot
	// has none (assault gadget 1: every launcher is unlocked by score) or progression is unknown.
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

// p_progression: the tables from read_unlock_progression (read here when null).
Result<KitDef> read_kit_def(const EbxResolver &p_db, std::string_view p_asset, const UnlockProgression *p_progression = nullptr);
// A TeamData asset (gameplay/teams/us).
Result<TeamDef> read_team_data(const EbxResolver &p_db, const EbxInstance &p_team_data);

} // namespace bf3::core::gameplay

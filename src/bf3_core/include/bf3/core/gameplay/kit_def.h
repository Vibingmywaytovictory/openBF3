#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

enum class WeaponSlot : uint8_t { Primary = 0, Sidearm = 1, Gadget1 = 2, Unused3 = 3, KitItem = 4, Gadget2 = 5, Grenade = 6, Knife = 7 };
inline constexpr size_t kWeaponSlotCount = 8;
const char *weapon_slot_name(WeaponSlot p_slot);

enum class UnlockGate : uint8_t { Free, Rank, Licence, Award, ScoringBucket };
const char *unlock_gate_name(UnlockGate p_gate);

struct UnlockProgression {

	std::map<Guid, UnlockGate> gates;
	size_t tables = 0;

	bool known() const { return tables != 0; }
	UnlockGate gate(const Guid &p_unlock) const;
};

Result<UnlockProgression> read_unlock_progression(const EbxResolver &p_db, WorkBudget *p_budget = nullptr);

struct KitUnlock {
	std::string unlock;
	std::string type;
	std::string weapon;
	UnlockGate gate = UnlockGate::Free;
	bool variant = false;
	uint32_t identifier = 0;
};

struct KitSlot {
	std::string ui_category_sid;
	std::vector<KitUnlock> unlocks;
};

struct KitDef {
	std::string asset;
	std::string label_sid;
	std::string hud_icon;
	int32_t hud_icon_value = 0;
	std::vector<KitSlot> weapon_slots;
	std::vector<KitUnlock> specializations;
	std::string specialization_category_sid;
	size_t visual_unlocks = 0;
	bool progression_known = false;

	const KitSlot *slot(WeaponSlot p_slot) const;

	const KitUnlock *default_unlock(WeaponSlot p_slot) const;
	const KitUnlock *default_specialization() const;
};

struct TeamDef {
	int team = 0;
	std::string team_data;
	std::string faction;
	std::string soldier;
	std::vector<std::string> kits;
	std::vector<std::string> vehicle_customizations;
};

Result<KitDef> read_kit_def(const EbxResolver &p_db, std::string_view p_asset, const UnlockProgression *p_progression = nullptr,
		WorkBudget *p_budget = nullptr);

Result<std::vector<KitSlot>> read_weapon_customization(const EbxResolver &p_db, std::string_view p_weapon_unlock,
		const UnlockProgression &p_progression, WorkBudget *p_budget = nullptr);

Result<TeamDef> read_team_data(const EbxResolver &p_db, const EbxInstance &p_team_data, WorkBudget *p_budget = nullptr);

}

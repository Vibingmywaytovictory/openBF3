#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr std::string_view kWeaponSwitchingPartition = "input/soldierweaponswitching";
inline constexpr std::string_view kThrowGrenadeAction = "EIAThrowGrenade";
inline constexpr uint8_t kNoSwitchSlot = 0xFF;

struct DirectWeaponSwitch {
	std::string action;
	uint8_t to_slot = kNoSwitchSlot;
	bool fire_and_switch_back = false;
	bool quick_switch = false;
};

struct WeaponSwitch {
	std::string action;
	uint8_t from_slot = kNoSwitchSlot;
	std::vector<uint8_t> to_slots;
	bool fire_and_switch_back = false;
};

struct WeaponSwitching {
	std::vector<DirectWeaponSwitch> direct;
	std::vector<WeaponSwitch> map;

	const DirectWeaponSwitch *direct_of(std::string_view p_action) const;
	const WeaponSwitch *switch_of(std::string_view p_action, uint8_t p_from_slot) const;
	bool switches_back(std::string_view p_action, uint8_t p_from_slot) const;
};

Result<WeaponSwitching> read_weapon_switching(const EbxResolver &p_db, WorkBudget *p_budget = nullptr);

}

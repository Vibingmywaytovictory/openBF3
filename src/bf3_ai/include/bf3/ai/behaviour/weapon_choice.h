#pragma once

#include <cstdint>
#include <span>

namespace bf3::ai {

enum class WeaponType : uint8_t { Default = 0, Heal = 1, Ammo = 2, Resurrect = 3, C4 = 4 };

struct WeaponChoice {
	WeaponType type = WeaponType::Default;
	bool indirect = false;
	int ammo = 0;
	int strength = 0;
	float min_range = 0.0f;
	float max_range = 0.0f;
};

int chose_weapon(std::span<const WeaponChoice> p_weapons, int p_current, float p_distance, bool p_no_indirect,
		bool p_heal, bool p_resurrect, bool p_ammo);

}

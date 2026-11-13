#include "bf3/ai/behaviour/weapon_choice.h"

namespace bf3::ai {

int chose_weapon(std::span<const WeaponChoice> p_weapons, int p_current, float p_distance, bool p_no_indirect,
		bool p_heal, bool p_resurrect, bool p_ammo) {
	int best = -1;
	int best_strength = 0;
	bool in_range = false;
	for (size_t i = 0; i < p_weapons.size(); ++i) {
		const WeaponChoice &w = p_weapons[i];
		if ((w.type == WeaponType::Resurrect) != p_resurrect || (w.type == WeaponType::Heal) != p_heal ||
				(w.type == WeaponType::Ammo) != p_ammo) {
			continue;
		}
		if (p_no_indirect && w.indirect) {
			continue;
		}
		if (w.ammo == 0 && !p_ammo && !p_heal && !p_resurrect) {
			continue;
		}
		if (w.strength <= 0) {
			continue;
		}
		if (w.max_range > p_distance && w.min_range < p_distance) {
			if (in_range && w.strength < best_strength) {
				continue;
			}
			in_range = true;
			best = int(i);
			best_strength = w.strength;
			if (best == p_current) {
				best_strength *= 3;
			}
		} else if (!in_range && w.strength > best_strength) {
			best = int(i);
			best_strength = w.strength;
			if (best == p_current) {
				best_strength *= 3;
			}
		}
	}
	return best;
}

}

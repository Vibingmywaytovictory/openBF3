#pragma once

#include "bf3/anim/crowd_eval.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/render/crowd_weapons.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr size_t kCrowdPreloadWeapons = 8;

struct CrowdItemLists {
	std::array<std::vector<render::CrowdWeaponSpec>, 2> teams;
	std::vector<std::string> notes;
};

inline render::CrowdWeaponSpec catalog_crowd_spec(const host::GunplayLevel &p_gunplay, uint16_t p_def) {
	render::CrowdWeaponSpec s;
	s.def = p_def;
	s.weapon_unlock = p_gunplay.defs[p_def].blueprint;
	s.weapon_def = p_gunplay.defs[p_def].variant;
	return s;
}

inline CrowdItemLists crowd_item_specs(const host::GunplayLevel *p_gunplay, const render::SoldierSpec &p_spec) {
	CrowdItemLists out;
	for (size_t t = 0; t < out.teams.size(); ++t) {
		std::vector<render::CrowdWeaponSpec> &items = out.teams[t];
		const render::SoldierTeamSpec &ts = p_spec.teams[t];
		render::CrowdWeaponSpec first;
		first.def = sim::weapons::kNoWeapon;
		first.mesh = ts.weapon_mesh;
		first.weapon_unlock = ts.weapon_unlock;
		first.weapon_def = ts.weapon_def;
		if (p_gunplay != nullptr) {
			for (size_t d = 0; d < p_gunplay->defs.size() && d < sim::weapons::kNoWeapon; ++d) {
				if (p_gunplay->defs[d].variant == ts.weapon_def) {
					first.def = uint16_t(d);
					break;
				}
			}
		}
		if (first.def == sim::weapons::kNoWeapon) {
			out.notes.push_back("team " + std::to_string(t) + ": " + ts.weapon_def + " is not a match def (item 0 has no def)");
		}
		items.push_back(first);
		if (p_gunplay == nullptr) {
			continue;
		}
		for (uint8_t k = 0; k < host::kKitCount; ++k) {
			const host::KitLoadout &kit = p_gunplay->kits.loadouts[t][k];
			for (const uint8_t slot : host::kKitSlots) {
				if (slot >= kit.defs.size()) {
					continue;
				}
				const uint16_t d = kit.defs[slot];
				if (d == sim::weapons::kNoWeapon || d >= p_gunplay->defs.size()) {
					continue;
				}
				bool listed = false;
				for (const render::CrowdWeaponSpec &s : items) {
					listed = listed || s.def == d;
				}
				if (listed) {
					continue;
				}
				if (items.size() >= kCrowdPreloadWeapons) {
					out.notes.push_back("team " + std::to_string(t) + ": def " + std::to_string(d) + " (" +
							p_gunplay->defs[d].variant + ") past " + std::to_string(kCrowdPreloadWeapons) +
							" preloaded items: loaded when first carried");
					continue;
				}
				items.push_back(catalog_crowd_spec(*p_gunplay, d));
			}
		}
	}
	return out;
}

}

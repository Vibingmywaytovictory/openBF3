#include "bf3/host/spawn_effects.h"

#include "bf3/core/gameplay/kit_def.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace bf3::host {

namespace sw = sim::weapons;
namespace gp = core::gameplay;

namespace {

bool explosive_class(std::string_view p_class) {
	return p_class == "wcC4" || p_class == "wcATMine" || p_class == "wcAt";
}

}

int32_t spawn_magazines(const sw::WeaponSimDef &p_def, size_t p_slot, const gp::SpecializationEffects &p_effects) {
	if (p_def.weapon_class == "wcUgl") {
		return p_def.magazines + int32_t(p_effects[gp::SpecializationTarget::UnderslungGrenades]);
	}
	if (explosive_class(p_def.weapon_class)) {
		const double boosted =
				std::round(double(p_def.magazines) * double(p_effects[gp::SpecializationTarget::Explosives]));
		return int32_t(std::clamp(boosted, 0.0, double(sw::kMaxMagazines)));
	}
	if (p_slot == size_t(gp::WeaponSlot::Grenade)) {
		return p_def.magazines + int32_t(p_effects[gp::SpecializationTarget::Grenades]);
	}
	if (p_slot == size_t(gp::WeaponSlot::Primary) || p_slot == size_t(gp::WeaponSlot::Sidearm)) {
		return gp::boosted_magazines(p_def.magazines, p_effects);
	}
	return p_def.magazines;
}

void apply_spawn_effects(
		sw::SoldierWeaponsState &r_state, const sw::WeaponTable &p_table, const gp::SpecializationEffects &p_effects) {
	for (size_t i = 0; i < r_state.slots.size(); ++i) {
		sw::WeaponSlotState &x = r_state.slots[i];
		if (x.def == sw::kNoWeapon) {
			continue;
		}
		const sw::WeaponSimDef &d = p_table.def(x.def, x.variant);
		if (!d.unlimited_ammo) {
			x.reserve = spawn_magazines(d, i, p_effects) * d.magazine_capacity;
		}
	}
}

sim::MotorModifiers spawn_motor_modifiers(
		const sim::MotorParams &p_params, const gp::SpecializationTable &p_table, uint32_t p_mask) {
	sim::MotorModifiers out = sim::motor_modifiers(p_params);
	for (size_t i = 0; i < p_table.entries.size() && i < gp::kMaxSpecializationEntries; ++i) {
		if ((p_mask >> i & 1u) == 0) {
			continue;
		}
		for (const auto &[target, value] : p_table.entries[i].effects) {
			if (target == gp::SpecializationTarget::Sprint && value > 0.0f) {
				out.body_sprint_multiplier = value;
			}
		}
	}
	return out;
}

}

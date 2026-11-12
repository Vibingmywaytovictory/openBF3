#include "vehicle_hits.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace sw = sim::weapons;

void VehicleDamageSource::configure(const sw::WeaponTable &p_table, std::span<const core::gameplay::WeaponDef> p_defs) {
	materials_ = p_table.materials;
	bullet_.assign(p_table.defs.size(), 0);
	blast_.assign(p_table.defs.size(), std::nullopt);
	for (size_t i = 0; i < p_table.defs.size(); ++i) {
		bullet_[i] = p_table.defs[i].projectile.property;
		if (materials_ != nullptr && i < p_defs.size() && p_defs[i].projectile.explosion.material.present) {
			const sw::SurfacePair pair = materials_->pair_of(p_defs[i].projectile.explosion.material);
			if (pair.resolved) {
				blast_[i] = pair.property;
			}
		}
	}
}

float VehicleDamageSource::amount(const sw::DamageEvent &p_event, uint8_t p_vehicle_property) const {
	const bool blast = p_event.kind == sw::DamageKind::Blast || p_event.kind == sw::DamageKind::DudBlast;
	if (materials_ == nullptr || p_event.def >= bullet_.size()) {
		return blast ? p_event.base : p_event.base * p_event.penetration;
	}
	if (blast) {
		const std::optional<uint8_t> property = blast_[p_event.def];
		if (!property) {
			return p_event.base;
		}
		const core::gameplay::DamageRelation rel = materials_->relation(*property, p_vehicle_property);
		if (!rel.present) {
			return p_event.base;
		}
		return std::max(p_event.base * rel.protection_multiplier - rel.protection_threshold, 0.0f);
	}
	const core::gameplay::DamageRelation rel = materials_->relation(bullet_[p_event.def], p_vehicle_property);
	const float damage = p_event.base * (rel.present ? rel.protection_multiplier : 1.0f) * p_event.penetration;
	return rel.present && damage < rel.protection_threshold ? 0.0f : damage;
}

uint32_t vehicle_of_target(uint32_t p_target) {
	return p_target - sim::vehicles::kVehicleTargetIdBase;
}

void locate_strike(VehicleStrike &r_strike, const sw::ProjectileStepOutput &p_out, const sim::Vec3 &p_centre) {
	const sw::DamageEvent &e = r_strike.event;
	if (e.kind == sw::DamageKind::Blast || e.kind == sw::DamageKind::DudBlast) {
		for (const sw::DetonationEvent &d : p_out.detonations) {
			if (d.player_slot == e.attacker_slot && d.shot_index == e.shot_index) {
				r_strike.position = d.position;
				r_strike.direction = { p_centre[0] - d.position[0], p_centre[1] - d.position[1],
					p_centre[2] - d.position[2] };
				r_strike.located = true;
				r_strike.directional = true;
				return;
			}
		}
		return;
	}
	for (const sw::ImpactEvent &i : p_out.impacts) {
		if (i.kind == sw::ImpactKind::Target && i.target == e.target && i.player_slot == e.attacker_slot &&
				i.shot_index == e.shot_index && i.pellet == e.pellet) {
			r_strike.position = i.position;
			r_strike.direction = i.velocity;
			r_strike.located = true;
			r_strike.directional = true;
			return;
		}
	}
}

bool strike_before(const VehicleStrike &p_a, const VehicleStrike &p_b) {
	const sw::DamageEvent &a = p_a.event, &b = p_b.event;
	if (a.time != b.time) {
		return a.time < b.time;
	}
	if (a.attacker_slot != b.attacker_slot) {
		return a.attacker_slot < b.attacker_slot;
	}
	if (a.shot_index != b.shot_index) {
		return a.shot_index < b.shot_index;
	}
	if (a.pellet != b.pellet) {
		return a.pellet < b.pellet;
	}
	return a.target < b.target;
}

}

#include "tdm_bot_weapons_vehicles.h"

#include "bf3/core/math.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/gadgets.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::host::tdm_bot {

namespace {

namespace sw = sim::weapons;

const sw::GadgetDef *gadget_of(const Gunplay &p_gunplay, uint16_t p_def) {
	if (p_def == sw::kNoWeapon) {
		return nullptr;
	}
	for (const sw::GadgetDef &g : p_gunplay.level().gadgets) {
		if (g.def == p_def) {
			return &g;
		}
	}
	return nullptr;
}

uint16_t slot_def(const BotArms &p_arms, uint8_t p_slot) {
	return p_arms.weapons() != nullptr && p_slot < sw::kMaxWeaponSlots ? p_arms.weapons()->slots[p_slot].def
																	   : sw::kNoWeapon;
}

float vehicle_strength(
		const sw::WeaponSimDef &p_def, uint16_t p_index, const VehicleDamageSource &p_source, uint8_t p_property) {
	const sw::ProjectileSimDef &p = p_def.projectile;
	float strength = 0.0f;
	if (p.kind != core::gameplay::ProjectileKind::None) {
		sw::DamageEvent e;
		e.def = p_index;
		const bool missile = p.kind == core::gameplay::ProjectileKind::Missile;
		e.kind = missile || p.explosion.present ? sw::DamageKind::Direct : sw::DamageKind::Bullet;
		e.base = missile ? p.direct_damage : p.start_damage;
		strength = std::max(strength, p_source.amount(e, p_property));
	}
	if (p.explosion.present) {
		sw::DamageEvent e;
		e.def = p_index;
		e.kind = sw::DamageKind::Blast;
		e.base = p.explosion.blast_damage;
		strength = std::max(strength, p_source.amount(e, p_property));
	}
	return strength;
}

bool enemy_crew(const TdmShared &p_shared, const sim::vehicles::VehicleTarget &p_vehicle, uint8_t p_team) {
	for (uint8_t k = 0; k < p_vehicle.crew_count; ++k) {
		const uint32_t c = p_vehicle.crew[k];
		if (c < p_shared.teams.size() && c < p_shared.alive.size() && p_shared.teams[c] != 0 &&
				p_shared.teams[c] != p_team && p_shared.alive[c] != 0) {
			return true;
		}
	}
	return false;
}

const sim::vehicles::VehicleTarget *find_vehicle(const TdmShared &p_shared, uint32_t p_id) {
	for (const sim::vehicles::VehicleTarget &v : p_shared.host.view->vehicle_targets()) {
		if (v.target.id == p_id) {
			return &v;
		}
	}
	return nullptr;
}

bool own_packs(const Gunplay &p_gunplay, uint8_t p_slot, uint16_t p_def) {
	for (const sw::Deployable &d : p_gunplay.deployables()) {
		if (d.owner == p_slot && d.def == p_def && !d.owner_gone) {
			return true;
		}
	}
	return false;
}

}

ai::WeaponType weapon_type(const Gunplay &p_gunplay, uint16_t p_def) {
	if (p_def == sw::kNoWeapon) {
		return ai::WeaponType::Default;
	}
	if (p_def == p_gunplay.config().defib_def) {
		return ai::WeaponType::Resurrect;
	}
	if (p_def < p_gunplay.table().defs.size() && p_gunplay.table().defs[p_def].weapon_class == kRepairToolClass) {
		return ai::WeaponType::Heal;
	}
	const sw::GadgetDef *g = gadget_of(p_gunplay, p_def);
	if (g == nullptr) {
		return ai::WeaponType::Default;
	}
	if (g->kind == sw::GadgetKind::ExplosionPack && g->pack.remote_detonator) {
		return ai::WeaponType::C4;
	}
	if (g->kind == sw::GadgetKind::Supply) {
		return g->healing.active() ? ai::WeaponType::Heal : ai::WeaponType::Ammo;
	}
	return ai::WeaponType::Default;
}

float weapon_range(const Gunplay &p_gunplay, uint16_t p_def) {
	if (p_def >= p_gunplay.table().defs.size() || weapon_type(p_gunplay, p_def) == ai::WeaponType::C4) {
		return 0.0f;
	}
	const sw::ProjectileSimDef &p = p_gunplay.table().defs[p_def].projectile;
	return float(sw::units_to_seconds(p.time_to_live)) * std::max(p.muzzle_speed, p.max_speed);
}

std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> vehicle_weapon_choices(
		const Gunplay &p_gunplay, const VehicleDamageSource &p_source, const BotArms &p_arms, uint8_t p_property) {
	std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> out{};
	for (uint8_t s = 0; s < sw::kMaxWeaponSlots; ++s) {
		const sw::WeaponSimDef *d = p_arms.def(s);
		if (d == nullptr) {
			continue;
		}
		const uint16_t def = slot_def(p_arms, s);
		ai::WeaponChoice &c = out[s];
		c.type = weapon_type(p_gunplay, def);
		c.ammo = p_arms.rounds(s);
		c.max_range = weapon_range(p_gunplay, def);
		const sw::GadgetDef *g = gadget_of(p_gunplay, def);
		if (g == nullptr || c.type == ai::WeaponType::C4) {
			c.strength = int(std::lround(vehicle_strength(*d, def, p_source, p_property)));
		}
	}
	return out;
}

float hull_surface(const sim::vehicles::VehicleTarget &p_vehicle, const sim::Vec3 &p_from, sim::Vec3 &r_near) {
	float best = std::numeric_limits<float>::infinity();
	r_near = p_vehicle.target.bound_center;
	for (uint8_t k = 0; k < p_vehicle.target.capsule_count; ++k) {
		const sw::ZoneCapsule &c = p_vehicle.target.capsules[k];
		const sim::Vec3 ab = sub(c.b, c.a);
		const sim::Vec3 ap = sub(p_from, c.a);
		const float len = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
		const float t =
				len > 0.0f ? std::clamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len, 0.0f, 1.0f) : 0.0f;
		const sim::Vec3 on = add(c.a, ab[0] * t, ab[1] * t, ab[2] * t);
		const float d = distance(p_from, on) - c.radius;
		if (d < best) {
			best = d;
			r_near = on;
		}
	}
	return best;
}

sim::Vec3 beside_hull_point(const VehicleAim &p_aim, const sim::Vec3 &p_eye, const sim::Vec3 &p_feet, float p_gap) {
	const float radius = std::max(distance(p_eye, p_aim.near_point) - p_aim.surface, 0.0f);
	const float dx = p_feet[0] - p_aim.near_point[0], dz = p_feet[2] - p_aim.near_point[2];
	const float len = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
	const float out = radius + p_gap;
	return { p_aim.near_point[0] + dx / len * out, p_feet[1], p_aim.near_point[2] + dz / len * out };
}

std::optional<uint8_t> defib_slot(const Gunplay &p_gunplay, const BotArms &p_arms) {
	const uint16_t defib = p_gunplay.config().defib_def;
	for (const uint8_t s : kGadgetSlots) {
		if (defib != sw::kNoWeapon && slot_def(p_arms, s) == defib && p_arms.def(s) != nullptr) {
			return s;
		}
	}
	return std::nullopt;
}

void BotVehicleWeapons::new_life() {
	enemy_ = {};
	phase_ = ExplosivePhase::Approach;
	rounds_before_ = 0;
	detonate_at_ = 0;
}

const VehicleDamageSource &BotVehicleWeapons::source(const Gunplay &p_gunplay) {
	if (!configured_) {
		source_.configure(p_gunplay.table(), p_gunplay.level().defs);
		configured_ = true;
	}
	return source_;
}

bool BotVehicleWeapons::sense(TdmShared &r_shared, const BotArms &p_arms, const VehicleCheck &p_check) {
	TdmShared &sh = r_shared;
	const Gunplay *g = sh.host.gunplay;
	const std::span<const sim::vehicles::VehicleTarget> vehicles = sh.host.view->vehicle_targets();
	const uint8_t team = p_check.slot < sh.teams.size() ? sh.teams[p_check.slot] : 0;
	const VehicleAim previous = enemy_;
	enemy_ = {};
	if (g == nullptr || vehicles.empty() || p_arms.weapons() == nullptr || p_check.self == nullptr || team == 0) {
		phase_ = ExplosivePhase::Approach;
		return false;
	}
	const sim::bots::PerceptionParams &pp = sh.config->perception;
	const VehicleDamageSource &src = source(*g);
	const sim::CollisionWorld &world = sh.host.view->world();
	VehicleAim best;
	float best_d = std::numeric_limits<float>::infinity();
	for (const sim::vehicles::VehicleTarget &v : vehicles) {
		if (!enemy_crew(sh, v, team)) {
			continue;
		}
		const sim::Vec3 &centre = v.target.bound_center;
		const float d = distance(p_check.eye, centre);
		if (!(d <= pp.max_range_m)) {
			continue;
		}
		const bool held = previous.held() && previous.id == v.target.id;
		const bool committed = held && previous.type == ai::WeaponType::C4 && phase_ >= ExplosivePhase::Wait;
		if (!held &&
				std::fabs(wrap(yaw_of(centre[0] - p_check.eye[0], centre[2] - p_check.eye[2]) - p_check.view_yaw)) >
						0.5f * pp.fov_h_deg * core::kDegToRadF) {
			continue;
		}
		const bool remembered = held && p_check.tick - previous.seen <= sim::seconds_to_ticks(pp.forget_s);
		const bool visible = !world.ray_any(p_check.eye, sub(centre, p_check.eye), sim::query_mask::kProjectile);
		if (!committed && !remembered && !visible) {
			continue;
		}
		const uint8_t property = v.target.capsule_count > 0 ? v.target.capsules[0].property : uint8_t(0);
		const std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> choices =
				vehicle_weapon_choices(*g, src, p_arms, property);
		const int current = held && previous.weapon_slot < sw::kMaxWeaponSlots ? int(previous.weapon_slot) : -1;
		const int w = ai::chose_weapon(choices, current, d, false, false, false, false);
		if (w < 0) {
			continue;
		}
		const float rank = committed ? -1.0f : d;
		if (rank < best_d) {
			best_d = rank;
			best.id = v.target.id;
			best.centre = centre;
			best.weapon_slot = uint8_t(w);
			best.type = choices[size_t(w)].type;
			best.surface = hull_surface(v, p_check.eye, best.near_point);
			best.visible = visible;
			best.seen = visible || !held ? p_check.tick : previous.seen;
		}
	}
	if (!best.held()) {
		phase_ = ExplosivePhase::Approach;
		return false;
	}
	if (best.id == previous.id && best.type == previous.type) {
		best.since = previous.since;
		const sim::Tick dt = p_check.tick - seen_tick_;
		best.velocity = dt > 0
				? core::vec::scale(sub(best.centre, previous.centre), 1.0f / (float(dt) * sim::kTickSeconds))
				: previous.velocity;
	} else {
		best.since = p_check.tick;
		phase_ = ExplosivePhase::Approach;
	}
	seen_tick_ = p_check.tick;
	enemy_ = best;
	return true;
}

bool BotVehicleWeapons::track(TdmShared &r_shared, const sim::Vec3 &p_eye) {
	VehicleAim &a = enemy_;
	if (!a.held()) {
		return false;
	}
	const sim::vehicles::VehicleTarget *v = find_vehicle(r_shared, a.id);
	if (v == nullptr) {
		return false;
	}
	a.centre = v->target.bound_center;
	a.surface = hull_surface(*v, p_eye, a.near_point);
	return true;
}

void BotVehicleWeapons::explosive_step(TdmShared &r_shared, const BotArms &p_arms, uint8_t p_slot, sim::Tick p_tick) {
	const Gunplay *g = r_shared.host.gunplay;
	const uint8_t s = enemy_.weapon_slot;
	const sw::WeaponSimDef *d = p_arms.def(s);
	if (g == nullptr || d == nullptr || enemy_.type != ai::WeaponType::C4) {
		return;
	}
	const int32_t rounds = p_arms.rounds(s);
	switch (phase_) {
		case ExplosivePhase::Approach:
			if (enemy_.surface <= d->projectile.explosion.inner_radius && rounds > 0) {
				phase_ = ExplosivePhase::Place;
				rounds_before_ = rounds;
			}
			break;
		case ExplosivePhase::Place:
			if (rounds < rounds_before_) {
				phase_ = ExplosivePhase::Wait;
			}
			break;
		case ExplosivePhase::Wait:
			if (rounds == 0 ||
					(p_arms.weapons()->active == s && p_arms.weapons()->pending == 0xFF &&
							p_arms.weapons()->slots[s].phase == sw::WeaponPhase::Ready)) {
				phase_ = ExplosivePhase::BackOff;
			}
			break;
		case ExplosivePhase::BackOff:
			if (enemy_.surface >= kBackOffMetres) {
				phase_ = ExplosivePhase::Detonate;
				detonate_at_ = p_tick + sim::seconds_to_ticks(kDetonateDelayS);
			}
			break;
		case ExplosivePhase::Detonate:
			if (!own_packs(*g, p_slot, slot_def(p_arms, s))) {
				phase_ = ExplosivePhase::Approach;
			}
			break;
	}
}

sim::Vec3 BotVehicleWeapons::back_off_goal(const sim::Vec3 &p_from, float p_turn) const {
	const float yaw = yaw_of(p_from[0] - enemy_.centre[0], p_from[2] - enemy_.centre[2]) + p_turn;
	return { enemy_.centre[0] + std::sin(yaw) * kBackOffGoalMetres, p_from[1],
		enemy_.centre[2] + std::cos(yaw) * kBackOffGoalMetres };
}

}

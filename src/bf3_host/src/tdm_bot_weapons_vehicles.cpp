#include "tdm_bot_weapons_vehicles.h"

#include "bf3/core/math.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/gadgets.h"
#include "bot_weapon_templates.h"
#include "tdm_bot_support.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::host::tdm_bot {

namespace {

namespace sw = sim::weapons;

uint16_t slot_def(const BotArms &p_arms, uint8_t p_slot) {
	return p_arms.weapons() != nullptr && p_slot < sw::kMaxWeaponSlots ? p_arms.weapons()->slots[p_slot].def
																	   : sw::kNoWeapon;
}

uint8_t hull_property(const sim::vehicles::VehicleTarget &p_vehicle) {
	return p_vehicle.target.capsule_count > 0 ? p_vehicle.target.capsules[0].property : uint8_t(0);
}

ai::CombatStrengthType vehicle_type(const TdmShared &p_shared, uint8_t p_property) {
	return p_shared.weapons != nullptr ? p_shared.weapons->vehicle_type(p_property)
									   : ai::CombatStrengthType::LightArmour;
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
	return weapon_type_of(p_gunplay.level(), p_gunplay.config().defib_def, p_def);
}

const ai::WeaponTemplate &slot_template(const TdmShared &p_shared, const BotArms &p_arms, uint8_t p_slot) {
	static const ai::WeaponTemplate kNone;
	if (p_shared.weapons == nullptr || p_arms.def(p_slot) == nullptr) {
		return kNone;
	}
	const sw::WeaponSlotState &ss = p_arms.weapons()->slots[p_slot];
	return p_shared.weapons->of(ss.def, ss.variant);
}

float weapon_range(const TdmShared &p_shared, const BotArms &p_arms, uint8_t p_slot) {
	return slot_template(p_shared, p_arms, p_slot).max_range;
}

std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> template_choices(
		const TdmShared &p_shared, const BotArms &p_arms, ai::CombatStrengthType p_type) {
	std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> out{};
	const Gunplay *g = p_shared.host.gunplay;
	if (g == nullptr) {
		return out;
	}
	for (uint8_t s = 0; s < sw::kMaxWeaponSlots; ++s) {
		if (p_arms.def(s) != nullptr) {
			out[s] = template_choice(
					slot_template(p_shared, p_arms, s), p_type, weapon_type(*g, slot_def(p_arms, s)), p_arms.rounds(s));
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

std::optional<uint8_t> defib_slot(const TdmShared &p_shared, const BotArms &p_arms) {
	const int s = ai::chose_weapon(
			template_choices(p_shared, p_arms, ai::CombatStrengthType::Infantry), -1, 0.0f, false, false, true, false);
	return s >= 0 ? std::optional<uint8_t>(uint8_t(s)) : std::nullopt;
}

std::optional<SoldierWeapon> soldier_weapon(const TdmShared &p_shared, const BotArms &p_arms, float p_distance) {
	const Gunplay *g = p_shared.host.gunplay;
	if (g == nullptr || p_arms.weapons() == nullptr) {
		return std::nullopt;
	}
	const std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> choices =
			template_choices(p_shared, p_arms, ai::CombatStrengthType::Infantry);
	const int w = ai::chose_weapon(choices, int(p_arms.weapons()->active), p_distance, false, false, false, false);
	if (w < 0) {
		return std::nullopt;
	}
	const uint8_t s = uint8_t(w);
	if (s == kSlotPrimary || s == kSlotSidearm) {
		return SoldierWeapon{ s, Trigger::Aimed };
	}
	if (s == kSlotKnife) {
		return SoldierWeapon{ s, Trigger::Knife };
	}
	const sw::WeaponSimDef *d = p_arms.def(s);
	if (choices[s].type != ai::WeaponType::Default || slot_template(p_shared, p_arms, s).is_thrown ||
			gadget_def(g->level(), slot_def(p_arms, s)) != nullptr || d == nullptr || !sw::fires_round(d->projectile)) {
		return std::nullopt;
	}
	return SoldierWeapon{ s, Trigger::Launcher };
}

void BotVehicleWeapons::new_life() {
	enemy_ = {};
	repair_ = {};
	phase_ = ExplosivePhase::Approach;
	rounds_before_ = 0;
	detonate_at_ = 0;
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
		const bool placing = held && previous.type == ai::WeaponType::C4 && phase_ >= ExplosivePhase::Place;
		int w = placing ? int(previous.weapon_slot) : -1;
		ai::WeaponType type = previous.type;
		if (!placing) {
			const std::array<ai::WeaponChoice, sw::kMaxWeaponSlots> choices =
					template_choices(sh, p_arms, vehicle_type(sh, hull_property(v)));
			const int current = held && previous.weapon_slot < sw::kMaxWeaponSlots ? int(previous.weapon_slot) : -1;
			w = ai::chose_weapon(choices, current, d, false, false, false, false);
			if (w < 0) {
				continue;
			}
			type = choices[size_t(w)].type;
		}
		const float rank = committed ? -1.0f : d;
		if (rank < best_d) {
			best_d = rank;
			best.id = v.target.id;
			best.centre = centre;
			best.weapon_slot = uint8_t(w);
			best.type = type;
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

bool BotVehicleWeapons::sense_repair(TdmShared &r_shared, const BotArms &p_arms, const VehicleCheck &p_check) {
	TdmShared &sh = r_shared;
	const Gunplay *g = sh.host.gunplay;
	const ServerView &view = *sh.host.view;
	const std::span<const sim::vehicles::VehicleTarget> vehicles = view.vehicle_targets();
	const uint8_t team = p_check.slot < sh.teams.size() ? sh.teams[p_check.slot] : 0;
	const VehicleAim previous = repair_;
	repair_ = {};
	if (g == nullptr || vehicles.empty() || p_arms.weapons() == nullptr || p_check.self == nullptr || team == 0) {
		return false;
	}
	VehicleAim best;
	float best_score = 0.0f;
	for (const sim::vehicles::VehicleTarget &v : vehicles) {
		if ((v.target.team != 0 && v.target.team != team) || enemy_crew(sh, v, team)) {
			continue;
		}
		bool own = false;
		bool human = false;
		for (uint8_t k = 0; k < v.crew_count; ++k) {
			own = own || v.crew[k] == p_check.slot;
			human = human || (v.crew[k] < 0xFF && !view.bot(uint8_t(v.crew[k])));
		}
		if (own || !ai::needs_assist(v.health, human)) {
			continue;
		}
		const float d = distance(p_check.self->position, v.target.bound_center);
		if (!(d <= ai::kAssistRadiusMetres)) {
			continue;
		}
		const int tool = ai::chose_weapon(
				template_choices(sh, p_arms, vehicle_type(sh, hull_property(v))), -1, 0.0f, false, true, false, false);
		if (tool < 0) {
			continue;
		}
		const float score = ai::assist_score(1.0f, v.health, std::max(d, 1.0f));
		if (score > best_score) {
			best_score = score;
			best.id = v.target.id;
			best.centre = v.target.bound_center;
			best.weapon_slot = uint8_t(tool);
			best.type = ai::WeaponType::Heal;
			best.surface = hull_surface(v, p_check.eye, best.near_point);
		}
	}
	if (!best.held()) {
		return false;
	}
	best.since = best.id == previous.id ? previous.since : p_check.tick;
	repair_ = best;
	return true;
}

bool BotVehicleWeapons::track(TdmShared &r_shared, const sim::Vec3 &p_eye, bool p_repair) {
	VehicleAim &a = p_repair ? repair_ : enemy_;
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
	const sw::SoldierWeaponsState &ws = *p_arms.weapons();
	ai::PlacedExplosiveCheck c;
	c.gap = enemy_.surface;
	c.reach = d->projectile.explosion.inner_radius;
	c.loaded = rounds > 0;
	c.thrown = rounds < rounds_before_;
	c.ready = ws.active == s && ws.pending == 0xFF && ws.slots[s].phase == sw::WeaponPhase::Ready;
	c.charges_out = phase_ == ExplosivePhase::Detonate && own_packs(*g, p_slot, slot_def(p_arms, s));
	const ExplosivePhase next = ai::placed_explosive_next(phase_, c);
	if (next == ExplosivePhase::Place && phase_ != next) {
		rounds_before_ = rounds;
	}
	if (next == ExplosivePhase::Detonate && phase_ != next) {
		detonate_at_ = p_tick + sim::seconds_to_ticks(ai::kPlacedDetonateDelayS);
	}
	phase_ = next;
}

sim::Vec3 BotVehicleWeapons::back_off_goal(const sim::Vec3 &p_from, float p_turn) const {
	const float yaw = yaw_of(p_from[0] - enemy_.centre[0], p_from[2] - enemy_.centre[2]) + p_turn;
	return { enemy_.centre[0] + std::sin(yaw) * ai::kPlacedBackOffMetres, p_from[1],
		enemy_.centre[2] + std::cos(yaw) * ai::kPlacedBackOffMetres };
}

}

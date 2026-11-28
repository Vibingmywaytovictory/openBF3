#include "bot_gunner.h"

#include "../bot_weapon_templates.h"
#include "../tdm_bot_body.h"
#include "../tdm_bot_common.h"
#include "../tdm_bot_shared.h"
#include "bf3/ai/main/bot_skill.h"
#include "bf3/core/math.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bot_crew.h"
#include "server_vehicles.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;
namespace vec = core::vec;

constexpr int kVehicleTargetId = 0x100;
// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviourPattern/BBPFireInfantery.cpp @c47317b, the fire step's 10 m
// delay range and 1 m splash precision; EntryTrigger.cpp @c47317b, the deviation scale of 3 against air.
constexpr float kFireDelayRangeMetres = 10.0f;
constexpr float kSplashSoldierPrecision = 1.0f;
constexpr float kAirTargetDeviationScale = 3.0f;

struct Seated {
	const ServerVehicles *vehicles = nullptr;
	VehicleSeat seat;
	uint16_t kind = 0;
	const LevelVehicleKind *model = nullptr;
};

std::optional<Seated> seated_in(const TdmShared &p_shared, uint8_t p_slot) {
	const ServerVehicles *vehicles = p_shared.host.view->server_vehicles();
	if (vehicles == nullptr || !vehicles->on() || p_shared.weapons == nullptr) {
		return std::nullopt;
	}
	const std::optional<VehicleSeat> seat = vehicles->seat_of(p_slot);
	if (!seat) {
		return std::nullopt;
	}
	Seated s;
	s.vehicles = vehicles;
	s.seat = *seat;
	s.kind = vehicles->level()->spawners[seat->vehicle].kind;
	s.model = &vehicles->level()->kinds[s.kind];
	return s.model->armament != nullptr ? std::optional<Seated>(s) : std::nullopt;
}

sim::Vec3 vehicle_velocity(const ServerVehicles &p_vehicles, size_t p_vehicle) {
	return p_vehicles.world()->state(sim::VehicleSlot(p_vehicle)).body.linear_velocity;
}

}

// INFERRED: a seated bot looks along its seat's first stance-0 weapon, else along the hull, as a BF2 seat senses from
// its camera. Settle: BF2's sensing direction for a vehicle seat.
std::optional<sim::Vec3> seat_view(const TdmShared &p_shared, uint8_t p_slot) {
	const ServerVehicles *vehicles = p_shared.host.view->server_vehicles();
	const std::optional<VehicleSeat> seat =
			vehicles != nullptr && vehicles->on() ? vehicles->seat_of(p_slot) : std::nullopt;
	if (!seat) {
		return std::nullopt;
	}
	const LevelVehicleKind &kind = vehicles->level()->kinds[vehicles->level()->spawners[seat->vehicle].kind];
	const core::Transform hull = vehicles->hull(seat->vehicle);
	for (size_t w = 0; kind.armament != nullptr && w < kind.armament->weapons.size(); ++w) {
		const sim::vehicles::ArmamentWeapon &weapon = kind.armament->weapons[w];
		if (size_t(weapon.seat) == seat->seat && sim::vehicles::weapon_in_stance(weapon, 0)) {
			const sim::vehicles::ArmamentState &state = vehicles->vehicles()[seat->vehicle].armament;
			return vec::normalize(sim::vehicles::weapon_world(*kind.armament, state, hull, w).forward);
		}
	}
	return vec::normalize(hull.forward);
}

void BotGunner::new_life() {
	sense_ = {};
	target_slot_ = -1;
	weapon_ = -1;
	target_vehicle_air_ = false;
	fired_target_ = -1;
	target_since_ = fire_at_ = last_command_ = 0;
	aimer_slot_ = -1;
}

ai::SeatFireSense BotGunner::evaluate(
		const TdmShared &p_shared, uint8_t p_slot, const sim::bots::Perception &p_perception) {
	const int previous = sense_.target;
	sense_ = {};
	target_slot_ = -1;
	weapon_ = -1;
	target_vehicle_air_ = false;
	const std::optional<Seated> in = seated_in(p_shared, p_slot);
	if (!in) {
		return sense_;
	}
	const ServerVehicles &vehicles = *in->vehicles;
	const ServerView &view = *p_shared.host.view;
	weapons_.clear();
	weapon_index_.clear();
	const std::vector<sim::vehicles::ArmamentWeapon> &arms = in->model->armament->weapons;
	for (size_t w = 0; w < arms.size(); ++w) {
		const ai::WeaponTemplate *t = p_shared.weapons->seat_weapon(in->kind, w);
		if (t == nullptr || size_t(arms[w].seat) != in->seat.seat) {
			continue;
		}
		// STAND-IN: every seat weapon counts as loaded and none as the active one, as the mount's magazine is not read.
		// Settle: feed the VehicleGunnery mount's magazine, reserve and selected weapon.
		ai::SeatFireWeapon f;
		f.strengths = t->strengths;
		f.min_range = t->min_range;
		f.max_range = t->max_range;
		weapons_.push_back(f);
		weapon_index_.push_back(w);
	}
	if (weapons_.empty()) {
		return sense_;
	}
	const std::vector<ai::ChangeVehicle> scene = change_vehicles(p_shared, vehicles, p_slot, false);
	const ai::ChangeVehicle &own = scene[in->seat.vehicle];
	const ai::ChangeSeat &own_seat = own.seat[std::min<size_t>(in->seat.seat, ai::kMaxChangeSeats - 1)];
	ai::SeatFireBot bot;
	bot.position = vehicles.seat_position(p_slot).value_or(own.position);
	bot.velocity = vehicle_velocity(vehicles, in->seat.vehicle);
	bot.strength_type = own.strength_type;
	bot.battle = own_seat.strengths;
	bot.exposed_soldier = own_seat.exposed;
	// INFERRED: the driver seat carries the vehicle's mobile part, as a BF2 vehicle's primary object does, and a gunner
	// seat is a static weapon whose camera test applies. Settle: BF2's IPIMobile on a secondary seat object.
	bot.mobile = in->seat.seat == 0;
	bot.max_speed = bot.mobile ? own.max_speed : 0.0f;
	// STAND-IN: the seat takes no ordered area and no attacker memory, so the area factor is 0.5 and the attack
	// factor 1. Settle: feed the SAI's ordered area and the bot's recent attackers.
	bot.current_target = previous;
	targets_.clear();
	target_slots_.clear();
	const std::span<const sim::SoldierState> states = view.states();
	const float full = p_shared.host.gunplay != nullptr ? p_shared.host.gunplay->level().max_health : 0.0f;
	for (const sim::bots::PerceivedEnemy &e : p_perception.enemies()) {
		if (!e.visible || e.slot >= states.size() || !view.alive(e.slot)) {
			continue;
		}
		const sim::SoldierState &s = states[e.slot];
		ai::SeatFireTarget t;
		const std::optional<VehicleSeat> theirs = vehicles.seat_of(e.slot);
		if (theirs && size_t(theirs->vehicle) < scene.size()) {
			const ai::ChangeVehicle &v = scene[theirs->vehicle];
			t.id = kVehicleTargetId + int(theirs->vehicle);
			if (std::any_of(targets_.begin(), targets_.end(),
						[&](const ai::SeatFireTarget &p_t) { return p_t.id == t.id; })) {
				continue;
			}
			t.position = v.position;
			t.velocity = vehicle_velocity(vehicles, theirs->vehicle);
			t.max_speed = v.max_speed;
			t.strength_type = v.strength_type;
			t.health = v.health;
			t.soldier = false;
			t.part_count = v.seats;
			for (uint8_t k = 0; k < v.seats; ++k) {
				t.parts[k] = { v.seat[k].strengths, v.seat[k].basic };
			}
		} else {
			t.id = int(e.slot);
			t.position = p_shared.chest_point(s);
			t.velocity = s.velocity;
			t.max_speed = p_shared.host.soldier_top_speed;
			t.health = full > 0.0f ? s.health / full : 0.0f;
			t.part_count = 1;
			t.parts[0] = { change_soldier_strengths(p_shared, e.slot), ai::kSoldierBasicStrength };
		}
		t.in_view = ai::change_camera_valid(
				own, in->seat.seat, vec::normalize_or(vec::sub(t.position, bot.position), own.forward));
		t.human = !view.bot(e.slot);
		targets_.push_back(t);
		target_slots_.push_back(e.slot);
	}
	const ai::SeatFireChoice choice = ai::choose_seat_fire(bot, weapons_, targets_);
	if (choice.target < 0) {
		return sense_;
	}
	sense_.urgency = choice.urgency;
	sense_.target = targets_[size_t(choice.target)].id;
	target_slot_ = target_slots_[size_t(choice.target)];
	weapon_ = int(weapon_index_[size_t(choice.weapon)]);
	const std::optional<VehicleSeat> theirs = vehicles.seat_of(uint8_t(target_slot_));
	target_vehicle_air_ = theirs && size_t(theirs->vehicle) < scene.size() &&
			scene[theirs->vehicle].kind == ai::ChangeUnitKind::Air && own.kind != ai::ChangeUnitKind::Naval &&
			in->seat.seat != 0;
	return sense_;
}

bool BotGunner::command(tdm_bot::BotBody &r_body, sim::Tick p_tick, bool p_fire, sim::Command &r_cmd) {
	TdmShared &r_shared = *r_body.shared;
	const uint8_t slot = r_body.slot;
	const std::optional<Seated> in = seated_in(r_shared, slot);
	const ServerView &view = *r_shared.host.view;
	if (!in || target_slot_ < 0 || weapon_ < 0 || !view.alive(uint8_t(target_slot_))) {
		return false;
	}
	const ServerVehicles &vehicles = *in->vehicles;
	const sw::WeaponTable *table = vehicles.gunnery().table();
	const size_t w = size_t(weapon_);
	const uint16_t def = w < in->model->weapon_defs.size() ? in->model->weapon_defs[w] : kNoVehicleWeapon;
	if (table == nullptr || def >= table->defs.size()) {
		return false;
	}
	const sim::SoldierState &ts = view.states()[size_t(target_slot_)];
	const std::optional<VehicleSeat> theirs = vehicles.seat_of(size_t(target_slot_));
	const bool soldier = !theirs;
	const sim::Vec3 target = soldier ? r_shared.chest_point(ts)
									 : vehicles.world()->state(sim::VehicleSlot(theirs->vehicle)).body.com_position;
	const sim::Vec3 target_velocity = soldier ? ts.velocity : vehicle_velocity(vehicles, theirs->vehicle);
	const core::Transform hull = vehicles.hull(in->seat.vehicle);
	const sim::vehicles::ArmamentState &armament = vehicles.vehicles()[in->seat.vehicle].armament;
	const core::Transform frame = sim::vehicles::weapon_world(*in->model->armament, armament, hull, w);
	const sim::Vec3 muzzle = sim::vehicles::weapon_muzzle(*in->model->armament, armament, hull, w);
	const sw::ProjectileSimDef &p = table->defs[def].projectile;
	const ai::WeaponTemplate *t = r_shared.weapons->seat_weapon(in->kind, w);
	if (t == nullptr) {
		return false;
	}
	// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviourPattern/BBPFireInfantery.cpp @c47317b: a fire plan that starts
	// fresh waits fire_delay_s before its first shot with a weapon reaching past 10 m; BBFire's target sets its time.
	if (sense_.target != fired_target_ || p_tick > last_command_ + 1u) {
		const bool running = fired_target_ >= 0 && p_tick <= last_command_ + 1u;
		fired_target_ = sense_.target;
		target_since_ = p_tick;
		fire_at_ = ai::fresh_fire_plan(running, r_body.skill) && t->max_range > kFireDelayRangeMetres
				? p_tick + sim::seconds_to_ticks(ai::fire_delay_s(r_body.skill, tdm_bot::unit(r_body.rng)))
				: p_tick;
	}
	last_command_ = p_tick;
	const float range = vec::length(vec::sub(target, muzzle));
	ai::Vec3 aim = vec::normalize_or(vec::sub(target, muzzle), frame.forward);
	if (!p.instant_hit) {
		if (aimer_slot_ != target_slot_) {
			aimer_ = ai::Aimer{};
			aimer_slot_ = target_slot_;
		}
		// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIPlanInterpreter/EntryMouseTurretAimAt.cpp @c47317b (accuracy 0.5).
		// WHY: no shooter velocity is passed, as openBF3's shots do not inherit the shooter's velocity.
		ai::Vec3 dir{};
		if (ai::aim_vector(aimer_, muzzle, target, target_velocity, {}, ai::kSoldierAimAccuracy, p.drag, p.muzzle_speed,
					p.gravity, double(p_tick) * sim::kTickSeconds, dir)) {
			aim = dir;
		}
	}
	// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIPlanInterpreter/EntryTrigger.cpp @c47317b: a vehicle's fire takes the
	// bot skill's deviation through setBotSkill, timed from when the target was set.
	ai::TriggerWeapon trigger;
	trigger.thrown = t->is_thrown;
	trigger.explosion_radius = t->explosion_radius;
	// INFERRED: every vehicle target is an occupied human vehicle, as the pin's bot lookup finds no bot for a vehicle.
	// Settle: BF2's IBotManager::getBot on a vehicle's AI object.
	const bool human_vehicle = !soldier;
	// STAND-IN: the seat templates' deviation is the pin's WeaponTemplate default and no seat weapon counts as guided.
	// Settle: a per-weapon deviation and guidance type from BF3's weapon data or retail footage.
	const float sigma = ai::ai_deviation_deg(*t, ai::trigger_skill(r_body.skill, trigger, false, human_vehicle),
								double(p_tick - target_since_) * sim::kTickSeconds,
								target_vehicle_air_ ? kAirTargetDeviationScale : 1.0f) *
			core::kDegToRadF;
	const tdm_bot::AimError err = r_body.targeting.aim_error(p_tick, sigma, r_body.rng);
	const float yaw = std::atan2(aim[0], aim[2]) + err.yaw;
	const float pitch = std::asin(std::clamp(aim[1], -1.0f, 1.0f)) + err.pitch;
	aim = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
	r_cmd.yaw = sim::quantize_yaw(yaw);
	r_cmd.pitch = sim::quantize_pitch(pitch);
	if (!p_fire || p_tick < fire_at_) {
		return true;
	}
	const sim::Vec3 own = vehicles.seat_position(slot).value_or(muzzle);
	const bool close_vehicle = !soldier && std::fabs(target[1] - own[1]) < ai::kPrecisionCloseHeight &&
			vec::length(vec::sub(target, own)) < ai::kPrecisionCloseMetres;
	// STAND-IN: the precision is the pin's WeaponTemplate default allowedDeviation (0.5 m), as BF3 data has none, and
	// BBPFireInfantery's 1 m for a vehicle's splash weapon at a soldier. Settle: a per-weapon precision from BF3 data.
	const float precision = soldier && t->explosion_radius != 0.0f ? kSplashSoldierPrecision : t->allowed_deviation;
	if (!ai::fire_precise(vec::normalize(frame.forward), aim, range, precision, close_vehicle)) {
		return true;
	}
	// STAND-IN: a single-shot or burst seat weapon has its trigger let go every other tick, openBF3's own press rule.
	// Settle: BF2's Trigger and TriggerContinously entries for a single-shot weapon.
	const bool automatic =
			table->defs[def].mode_count > 0 && table->defs[def].modes[0].logic == core::gameplay::FireLogic::Automatic;
	if (automatic || p_tick % 2u == 0u) {
		r_cmd.buttons |= sim::buttons::kFire;
		r_shared.count(&TdmBotStats::vehicle_fire_ticks);
	}
	return true;
}
}

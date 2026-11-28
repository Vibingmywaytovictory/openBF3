#include "bot_gunner.h"

#include "../bot_weapon_templates.h"
#include "../tdm_bot_shared.h"
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
	aimer_slot_ = -1;
}

ai::SeatFireSense BotGunner::evaluate(
		const TdmShared &p_shared, uint8_t p_slot, const sim::bots::Perception &p_perception) {
	const int previous = sense_.target;
	sense_ = {};
	target_slot_ = -1;
	weapon_ = -1;
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
	return sense_;
}

bool BotGunner::command(TdmShared &r_shared, uint8_t p_slot, sim::Tick p_tick, bool p_fire, sim::Command &r_cmd) {
	const std::optional<Seated> in = seated_in(r_shared, p_slot);
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
	r_cmd.yaw = sim::quantize_yaw(std::atan2(aim[0], aim[2]));
	r_cmd.pitch = sim::quantize_pitch(std::asin(std::clamp(aim[1], -1.0f, 1.0f)));
	if (!p_fire) {
		return true;
	}
	const ai::WeaponTemplate *t = r_shared.weapons->seat_weapon(in->kind, w);
	const sim::Vec3 own = vehicles.seat_position(p_slot).value_or(muzzle);
	const bool close_vehicle = !soldier && std::fabs(target[1] - own[1]) < ai::kPrecisionCloseHeight &&
			vec::length(vec::sub(target, own)) < ai::kPrecisionCloseMetres;
	if (t == nullptr ||
			!ai::fire_precise(vec::normalize(frame.forward), aim, range, t->allowed_deviation, close_vehicle)) {
		return true;
	}
	// WHY: a single-shot or burst seat weapon fires on the trigger's press, so the trigger is let go every other tick.
	const bool automatic =
			table->defs[def].mode_count > 0 && table->defs[def].modes[0].logic == core::gameplay::FireLogic::Automatic;
	if (automatic || p_tick % 2u == 0u) {
		r_cmd.buttons |= sim::buttons::kFire;
		r_shared.count(&TdmBotStats::vehicle_fire_ticks);
	}
	return true;
}

}

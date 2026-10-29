#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

namespace bf3::ai {

IPIUnit::IPIUnit(Information *p_information) : InformationPlugIn(p_information) {
	ammo_ = ammo();
	strength_mod_ = 1.0f;
	set_time();
}

AiObjectUnit &IPIUnit::object() const {
	return *information_->ai_object()->unit();
}

void IPIUnit::set_time() {
	set_time(information_->context().now());
}

void IPIUnit::set_time(sim::Tick p_time) {
	const IPIPhysical *const physical = information_->physical();
	ammo(p_time);
	if (physical != nullptr) {
		calculate_strength_mod(physical->health());
	}
	calculate_strengths();
}

void IPIUnit::calculate_strengths() {
	const AiObjectUnit &unit = object();
	if (strength_mod_ < 0.0f) {
		strength_mod_ = 0.0f;
	} else if (strength_mod_ > 1.0f) {
		strength_mod_ = 1.0f;
	}
	const int *const battle = unit.battle_strengths();
	for (int i = 0; i < kBattleStrengthCount; ++i) {
		battle_strengths_[i] = float(battle[i]) * strength_mod_;
	}
	const int *const strategic = unit.strategic_strengths();
	for (int i = 0; i < kStrategicStrengthCount; ++i) {
		strategic_strengths_[i] = float(strategic[i]) * strength_mod_;
	}
}

float IPIUnit::ammo() {
	return ammo(information_->context().now());
}

float IPIUnit::ammo(sim::Tick p_time) {
	if (ammo_time_.has_value() && *ammo_time_ >= p_time) {
		return ammo_;
	}
	ammo_time_ = p_time;
	const IPIArmament *const armament = information_->armament();
	const WeaponInfoManager *const manager = information_->context().weapons;
	if (armament == nullptr || manager == nullptr) {
		return 1.0f;
	}
	float total = 0.0f;
	for (const uint32_t handle : armament->weapons()) {
		if (handle == kNoHandle) {
			continue;
		}
		const WeaponInfo *const weapon = manager->information(handle);
		if (weapon == nullptr) {
			continue;
		}
		const float rounds = float(weapon->ammo());
		if (rounds < 0.0f) {
			return 1.0f;
		}
		total += rounds;
	}
	const int maximum = object().max_ammo();
	ammo_ = maximum > 0 ? total / float(maximum) : 1.0f;
	return ammo_;
}

bool IPIUnit::calculate_strength_mod(float p_health) {
	const float from_ammo = float(double(SCurve::calculate(ammo_)) * 0.5 + 0.25);
	const double modifier = double(SCurve::calculate(p_health)) * 0.25 + from_ammo;
	const bool changed = modifier != double(strength_mod_);
	strength_mod_ = float(modifier);
	return changed;
}

uint32_t IPIUnit::occupier(int p_side) const {
	const AiObject *const occupier = object().occupier();
	return occupier ? occupier->information(p_side) : kNoHandle;
}

uint32_t IPIUnit::occupied_object(int p_side) const {
	const AiObject *const occupied = object().occupied_object();
	return occupied ? occupied->information(p_side) : kNoHandle;
}

int IPIUnit::occupying_player_id() const {
	return object().occupying_player_id();
}

int IPIUnit::equipment_type() const {
	return object().equipment_type();
}

bool IPIUnit::is_occupied() const {
	return object().is_occupied();
}

bool IPIUnit::use_no_pathfinding_to_get_to_object() const {
	return object().use_no_pathfinding_to_get_to_object();
}

bool IPIUnit::has_exposed_soldier() const {
	return object().has_exposed_soldier();
}

bool IPIUnit::always_target_soldier() const {
	return object().always_target_soldier();
}

float IPIUnit::cull_distance() const {
	return object().cull_distance();
}

float IPIUnit::cull_distance_sqr() const {
	return object().cull_distance_sqr();
}

float IPIUnit::entry_radius() const {
	return object().entry_radius();
}

sim::Tick IPIUnit::last_player_possession_tick() const {
	return object().last_player_possession_tick();
}

int IPIUnit::team_lock() const {
	return object().team_lock();
}

bool IPIUnit::is_team_locked(int p_side) const {
	const int lock = object().team_lock();
	return lock == 0 || lock == p_side;
}

int IPIUnit::missile_lock_status() const {
	return object().missile_lock_status();
}

void IPIUnit::set_missile_lock_status(int p_status) {
	object().set_missile_lock_status(p_status);
}

bool IPIUnit::must_borrow_armament() const {
	return object().must_borrow_armament(information_->ai_object());
}

const Vec3 &IPIUnit::entry_position_of_non_mobile() const {
	return object().entry_position_of_non_mobile();
}

}

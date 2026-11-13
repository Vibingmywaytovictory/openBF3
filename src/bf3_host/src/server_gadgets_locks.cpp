#include "server_gadgets.h"

#include "bf3/core/math.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/vehicles/vehicle_targets.h"

#include <algorithm>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;

const sw::WeaponLockState kNoLock{};

}

void LockGuidance::set(std::span<const sw::LockCandidate> p_candidates) {
	candidates_.assign(p_candidates.begin(), p_candidates.end());
}

std::optional<sw::GuidanceTarget> LockGuidance::guidance_target(uint32_t p_id) const {
	for (const sw::LockCandidate &c : candidates_) {
		if (c.id == p_id) {
			return sw::GuidanceTarget{ c.position, c.aim };
		}
	}
	return std::nullopt;
}

sw::LockCandidate lock_candidate_of(const LevelVehicleKind &p_kind, uint16_t p_spawner, uint8_t p_team,
		const sim::Vec3 &p_com, const std::array<float, 4> &p_rotation, bool p_occupied) {
	sw::LockCandidate c = p_kind.lock;
	const sim::Vec3 com = p_kind.model != nullptr ? p_kind.model->center_of_mass : sim::Vec3{};
	const sim::Vec3 origin = core::vec::sub(p_com, sim::vehicles::hull_transform(p_rotation, {}).transform_vector(com));
	c.id = sim::vehicles::kVehicleTargetIdBase + p_spawner;
	c.team = p_team;
	c.position = origin;
	c.aim = sim::vehicles::hull_transform(p_rotation, origin).transform_point(p_kind.box.centre);
	c.occupied = p_occupied;
	return c;
}

void ServerGadgets::set_lock_candidates(std::span<const sw::LockCandidate> p_candidates) {
	guidance_.set(p_candidates);
}

bool ServerGadgets::aim_lock(uint8_t p_slot, uint8_t p_team, uint16_t p_def, uint8_t p_variant, const sim::Vec3 &p_eye,
		const sim::Vec3 &p_forward, bool p_zoom, const sim::CollisionWorld *p_world) {
	if (p_slot >= locks_.size()) {
		return false;
	}
	sw::WeaponLockState &state = locks_[p_slot];
	if (table_ == nullptr || p_def >= table_->defs.size() || !table_->def(p_def, p_variant).locking.present) {
		state = sw::WeaponLockState();
		lock_defs_[p_slot] = sw::kNoWeapon;
		return false;
	}
	if (lock_defs_[p_slot] != p_def) {
		state = sw::WeaponLockState();
		lock_defs_[p_slot] = p_def;
	}
	const bool was = state.locked();
	sw::LockAim aim;
	aim.origin = p_eye;
	aim.forward = p_forward;
	aim.team = p_team;
	aim.zoom = p_zoom ? 1 : 0;
	sw::LockStepStats s;
	sw::step_weapon_lock(table_->def(p_def, p_variant).locking, state, aim, sw::kTimeUnitsPerTick,
			guidance_.candidates(), p_world, &s);
	stats_.lock_samples += s.samples;
	stats_.lock_candidates += s.candidates;
	stats_.lock_rays += s.rays;
	stats_.locks_gained += !was && state.locked() ? 1u : 0u;
	return state.locked();
}

uint32_t ServerGadgets::launch_target(uint8_t p_slot, uint16_t p_def, uint8_t p_variant) {
	if (p_slot >= locks_.size() || table_ == nullptr || p_def >= table_->defs.size() || lock_defs_[p_slot] != p_def ||
			!table_->def(p_def, p_variant).locking.homing) {
		return sw::kNoLockTarget;
	}
	const uint32_t target = locks_[p_slot].target();
	stats_.guided_launches += target != sw::kNoLockTarget ? 1u : 0u;
	return target;
}

std::optional<net::OwnLock> own_lock_wire(const sw::WeaponLockState &p_lock) {
	const sw::LockState &a = p_lock.primary;
	const sw::LockState &b = p_lock.secondary;
	const bool second = !a.locked &&
			(b.locked || a.target == sw::kNoLockTarget || (b.target != sw::kNoLockTarget && b.amount > a.amount));
	const sw::LockState &shown = second ? b : a;
	net::OwnLock w;
	w.target = net::lock_wire(shown.target);
	if (w.target == net::kNoLockWire) {
		return std::nullopt;
	}
	w.amount = uint8_t(std::clamp(shown.amount, 0.0f, 1.0f) * 255.0f + 0.5f);
	w.locked = shown.locked;
	return w;
}

void ServerGadgets::reset_lock(uint8_t p_slot) {
	if (p_slot < locks_.size()) {
		locks_[p_slot] = sw::WeaponLockState();
		lock_defs_[p_slot] = sw::kNoWeapon;
	}
}

const sw::WeaponLockState &ServerGadgets::lock(uint8_t p_slot) const {
	return p_slot < locks_.size() ? locks_[p_slot] : kNoLock;
}

}

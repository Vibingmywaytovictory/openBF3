#pragma once

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/sim_time.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::sim::weapons {

using core::gameplay::LockType;

inline constexpr size_t kMaxZoomLocks = 4;
inline constexpr uint32_t kNoLockTarget = UINT32_MAX;
inline constexpr float kLockedAmount = 0.999999f;
inline constexpr float kLockSignatureFloor = 1e-6f;
inline constexpr float kLockConeFloor = 1e-6f;
inline constexpr float kLockMinDistance = 10.0f;
inline constexpr float kLockVisibilityBeyond = 3.0f;

struct LockingControllerSimDef {
	bool present = false;
	SimTime lock_time = 0;
	SimTime release_time = 0;
	SimTime release_on_new_target_time = 0;
	SimTime sample_rate = 0;
	SimTime minimum_lock_time = 0;
	float ray_length = 0;
	float acceptance_angle_deg = 0;
	float angle_constant = 0;
	float distance_constant = 0;
	bool lock_on_empty_vehicles = false;
	bool visible_only = false;
	bool ignore_height = false;
	bool position_only = false;
	std::array<LockType, kMaxZoomLocks> zoom_locks{};
	uint8_t zoom_lock_count = 0;

	LockType lock_type(uint8_t p_zoom) const { return p_zoom < zoom_lock_count ? zoom_locks[p_zoom] : LockType::Count; }
};

struct LockingSimDef {
	bool present = false;
	bool fire_only_when_locked = false;
	bool guided = false;
	bool guided_when_zoomed = false;
	bool homing = false;
	bool painter = false;
	SimTime painted_time = 0;
	LockingControllerSimDef primary;
	LockingControllerSimDef secondary;
};

struct GuidanceSimDef {
	bool present = false;
	float max_turn_deg_per_s = 0;
	SimTime activate = 0;
	std::array<float, 2> static_position{};
	std::array<float, 2> target_offset{};
	bool use_static = false;
	bool use_target = false;
	bool near_target = false;
	float near_radius = 0;
	SimTime near_min_delay = 0;
	SimTime near_max_delay = 0;
};

struct LockCandidate {
	uint32_t id = kNoLockTarget;
	uint8_t team = 0;
	Vec3 position{};
	Vec3 aim{};
	bool lockable = true;
	bool occupied = false;
	bool never_report_empty = false;
	float radar = 0;
	float heat = 0;
	bool painted = false;
	float time_multiplier = 1;
};

float lock_signature(const LockCandidate &p_candidate, LockType p_type);

struct LockAim {
	Vec3 origin{};
	Vec3 forward{ 0, 0, 1 };
	uint8_t team = 0;
	uint8_t zoom = 0;
};

struct LockState {
	uint32_t target = kNoLockTarget;
	SimTime timer = 0;
	SimTime release = 0;
	SimTime elapsed = 0;
	SimTime hold = 0;
	float amount = 0;
	float time_multiplier = 1;
	bool locked = false;
	uint32_t samples = 0;
};

struct LockStepStats {
	uint32_t samples = 0;
	uint32_t candidates = 0;
	uint32_t rays = 0;
};

const LockCandidate *pick_lock_candidate(const LockingControllerSimDef &p_def, LockType p_type, const LockAim &p_aim,
		std::span<const LockCandidate> p_candidates, const CollisionWorld *p_world, LockStepStats *r_stats = nullptr);

void step_lock(const LockingControllerSimDef &p_def, LockState &r_state, const LockAim &p_aim, SimTime p_dt,
		std::span<const LockCandidate> p_candidates, const CollisionWorld *p_world, LockStepStats *r_stats = nullptr);

struct WeaponLockState {
	LockState primary;
	LockState secondary;

	bool locked() const { return primary.locked || secondary.locked; }
	uint32_t target() const {
		return primary.locked ? primary.target : (secondary.locked ? secondary.target : kNoLockTarget);
	}
};

void step_weapon_lock(const LockingSimDef &p_def, WeaponLockState &r_state, const LockAim &p_aim, SimTime p_dt,
		std::span<const LockCandidate> p_candidates, const CollisionWorld *p_world, LockStepStats *r_stats = nullptr);

}

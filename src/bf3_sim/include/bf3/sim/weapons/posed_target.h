#pragma once
// A soldier's hit target posed from the capsule pose table (plan R14; docs/design/m2a.md §6.5; owner lc): the two
// nearest pitch buckets (and, during a stance transition, the two nearest transition fractions) lerped, rotated by
// the yaw (Command quantisation of the facing) and translated by the feet. O(1) per target, ~0.3 us for 9 capsules
// [inferred]. Both ends call it: the server's RewindProvider for hits, the client's cosmetic test (§8.4) on the
// same sample, so equal TargetPoses give equal HitTargets bit for bit. Until lc lands, the server uses wp's
// standin_target (hit_target.h), the only production fallback of §15.1.

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/hit_target.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim::weapons {

struct TargetPose {
	Vec3 feet{}; // world
	float yaw = 0; // radians, sim convention (command.h)
	float pitch = 0; // radians, + = up; clamped to the table's pitch range
	Pose pose = Pose::Stand; // the pose being left during a transition (SoldierState::pose)
	Pose target_pose = Pose::Stand; // == pose when no transition runs
	float transition = 0; // 0..1 progress of pose -> target_pose (net::OtherSoldier::transition / 255)
	uint32_t id = 0; // HitTarget::id (the server: the player slot)
	uint32_t team = 0;
	bool spawn_protected = false;
};
static_assert(std::is_trivially_copyable_v<TargetPose>);

// p_table must pass core::gameplay::capsule_pose_table_valid (checked once at load, not per call).
HitTarget posed_target(const core::gameplay::CapsulePoseTable &p_table, const TargetPose &p_pose);

} // namespace bf3::sim::weapons

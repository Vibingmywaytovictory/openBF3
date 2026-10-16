#pragma once
// A soldier's hit target posed from the capsule pose table (plan R14; docs/design/m2a.md §6.5; owner lc): the two
// nearest pitch buckets (and, during a stance transition, the two nearest transition fractions) lerped, rotated by
// the yaw (Command quantisation of the facing) and translated by the feet. O(1) per target, ~0.3 us for 9 capsules
// [inferred]. Both ends call it: the server's RewindProvider for hits, the client's cosmetic test (§8.4) on the
// same sample, so equal TargetPoses give equal HitTargets bit for bit. Until lc lands, the server uses wp's
// standin_target (hit_target.h), the only production fallback of §15.1.

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/hit_target.h"

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

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

// ---- lc additions (as built; docs/design/m2a-lc.md) ----------------------------------------------------------------

// The material side of the capsule pose table's inputs (bf3_anim's CapsuleTableInputs takes them as plain data, since
// bf3_anim does not link bf3_sim): per capsule of the BoneCollisionData list (all of them, in list order) the pair's
// grid property and its zone (the pair's property name, standin_target's rule; a capsule outside the hi-LOD set whose
// pair has no zone gets Body), and per stance wp's standin_target layout at feet (0, 0, 0), yaw 0, which is the local
// frame (x left, y up, z forward), the hi-LOD capsules in list order. Fails as standin_target fails.
struct CapsuleTableData {
	std::vector<uint8_t> property;
	std::vector<uint8_t> zone;
	std::array<std::vector<core::gameplay::CapsuleLocal>, core::gameplay::kCapsuleStances> standin;
};
core::Result<CapsuleTableData> capsule_table_data(const core::gameplay::SoldierDef &p_soldier, std::span<const core::gameplay::HitCapsule> p_capsules,
		const SurfaceMaterialTable &p_materials);

} // namespace bf3::sim::weapons

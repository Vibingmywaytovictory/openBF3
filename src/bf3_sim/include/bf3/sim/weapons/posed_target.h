#pragma once

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
	Vec3 feet{};
	float yaw = 0;
	float pitch = 0;
	Pose pose = Pose::Stand;
	Pose target_pose = Pose::Stand;
	float transition = 0;
	uint32_t id = 0;
	uint32_t team = 0;
	bool spawn_protected = false;
};
static_assert(std::is_trivially_copyable_v<TargetPose>);

HitTarget posed_target(const core::gameplay::CapsulePoseTable &p_table, const TargetPose &p_pose);

struct CapsuleTableData {
	std::vector<uint8_t> property;
	std::vector<uint8_t> zone;
	std::array<std::vector<core::gameplay::CapsuleLocal>, core::gameplay::kCapsuleStances> standin;
};
core::Result<CapsuleTableData> capsule_table_data(const core::gameplay::SoldierDef &p_soldier, std::span<const core::gameplay::HitCapsule> p_capsules,
		const SurfaceMaterialTable &p_materials);

}

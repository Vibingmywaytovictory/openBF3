#pragma once

#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bf3::core::ant {
class AntLibrary;
}

namespace bf3::anim {

struct HandReachArm {
	int32_t arm = -1, fore = -1, hand = -1, effector = -1;
	float reach = 0.0f;
	int32_t shoulder = -1;

	bool usable(size_t p_joints) const;
};

struct HandReach {
	std::array<HandReachArm, 2> arms;

	bool any(size_t p_joints) const;
};

core::Result<HandReach> read_hand_reach(
		const core::ant::AntLibrary &p_library, const Skeleton &p_rig, std::string_view p_rig_name);

void reach_hands(const Skeleton &p_rig, const HandReach &p_reach, const std::vector<Xform> &p_held, LocalPose &r_pose,
		std::vector<Xform> &r_model);

}

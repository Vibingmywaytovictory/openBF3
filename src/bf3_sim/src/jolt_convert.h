#pragma once

#include "bf3/sim/sim_types.h"

#include <Jolt/Jolt.h>

#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>

#include <array>

namespace bf3::sim {

inline JPH::Vec3 to_jolt(const Vec3 &p_v) {
	return JPH::Vec3(p_v[0], p_v[1], p_v[2]);
}

inline Vec3 from_jolt(JPH::Vec3Arg p_v) {
	return { p_v.GetX(), p_v.GetY(), p_v.GetZ() };
}

inline JPH::Quat to_jolt(const std::array<float, 4> &p_q) {
	return JPH::Quat(p_q[0], p_q[1], p_q[2], p_q[3]);
}

}

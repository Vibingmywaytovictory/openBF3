#pragma once
// Run blend for the 3p crowd: speed and direction to weights over the idle pose and four direction cycles,
// with one shared normalized phase (S3; docs/design/s3.md §5.3).
//
// Clips [verified 2026-10-15, Ant package b_basicassetsmp, each controller name unique in the package, target
// FullBodyWeaponRoot, DCT, FPS 30, FPSScale 0.5, NumTicks = 2 * EndFrame, Cycle flag false]:
//   Rifle_StandIdle_Pose  Raw, end frame 1 (a pose)
//   Rifle_RunFwd   end frame 18   root +2.784 m along z over 0.60 s  = 4.64 m/s
//   Rifle_RunBwd   end frame 17   root -1.686 m along z over 0.567 s = 2.97 m/s
//   Rifle_RunLeft1 end frame 17   root +1.472 m along x              ~ 2.6 m/s
//   Rifle_RunRight1 end frame 16  root -1.71 m along x               ~ 3.2 m/s
// The root motion lies on the AITrajectory joint's translation. The simulation owns the root (M1 motor:
// 4.0 m/s forward, 0.75x back and strafe), so evaluation removes the AITrajectory translation
// (CrowdClip::root_joint) and plays each cycle at speed / root speed. The direction of a cycle is its MEASURED
// root displacement (LocoClipInfo::root_dir). No axis convention is assumed: the local velocity is matched
// against those vectors. Cycles loop by wrapping. First and last frames agree within 0.16 deg (RunFwd) to
// 2.27 deg (RunRight1), excluding root motion.

#include "bf3/anim/pose_math.h"

#include <array>
#include <cstdint>

namespace bf3::anim {

enum class LocoClip : uint8_t { Idle, Fwd, Bwd, Left, Right, Count };
constexpr size_t kLocoClips = size_t(LocoClip::Count);

struct LocoClipInfo {
	float duration = 0.0f; // seconds (end_frame / fps); 0 for the idle pose
	float root_speed = 0.0f; // m/s of the root displacement over the cycle; 0 for idle
	Vec3 root_dir{}; // unit horizontal direction of that displacement in rig model space (y = 0)
};

struct LocoParams {
	float idle_speed = 0.2f; // m/s: at or below this, idle only
	float full_speed = 1.0f; // m/s: at or above this, no idle weight (a linear fade in between)
	float min_rate = 0.5f; // playback-rate clamp, so a motor speed far from a clip's root speed stays readable
	float max_rate = 1.6f;
};

struct LocoWeights {
	std::array<float, kLocoClips> weight{}; // sums to 1; at most two direction cycles are nonzero, plus idle
	float cycle_seconds = 0.0f; // weighted cycle duration at the playback rate: phase advances dt / cycle_seconds
};

// Weights for a horizontal velocity in the soldier's local frame (p_local = rotate(-yaw) * world velocity,
// in rig model space): the two cycles whose root_dir bracket the velocity direction, by angle;
// idle by speed. Playback rate = speed / blended root speed, clamped to [min_rate, max_rate]. A non-finite
// velocity counts as zero (idle).
LocoWeights locomotion_weights(const std::array<LocoClipInfo, kLocoClips> &p_clips, const Vec3 &p_local, const LocoParams &p_params);

// Advances a normalized phase in [0, 1) by p_dt; the result stays in [0, 1) for any finite input and is 0 for
// non-finite input or cycle_seconds <= 0.
inline float advance_phase(float p_phase, float p_dt, float p_cycle_seconds) {
	if (!(p_cycle_seconds > 0.0f) || !(p_dt >= 0.0f) || !(p_phase >= 0.0f && p_phase < 1.0f) || p_dt > 1.0e6f) {
		return 0.0f;
	}
	float phase = p_phase + p_dt / p_cycle_seconds;
	if (!(phase < 1.0e9f)) {
		return 0.0f; // a tiny cycle or a huge dt: no meaningful phase (and no overflow in the cast below)
	}
	phase -= float(int64_t(phase));
	return phase >= 1.0f ? 0.0f : phase;
}

} // namespace bf3::anim

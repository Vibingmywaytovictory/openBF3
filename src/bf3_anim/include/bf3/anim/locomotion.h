#pragma once

#include "bf3/anim/pose_math.h"

#include <array>
#include <cstdint>

namespace bf3::anim {

enum class LocoClip : uint8_t { Idle, Fwd, Bwd, Left, Right, Count };
constexpr size_t kLocoClips = size_t(LocoClip::Count);

struct LocoClipInfo {
	float duration = 0.0f;
	float root_speed = 0.0f;
	Vec3 root_dir{};
};

struct LocoParams {
	float idle_speed = 0.2f;
	float full_speed = 1.0f;
	float min_rate = 0.5f;
	float max_rate = 1.6f;
};

struct LocoWeights {
	std::array<float, kLocoClips> weight{};
	float cycle_seconds = 0.0f;
};

LocoWeights locomotion_weights(const std::array<LocoClipInfo, kLocoClips> &p_clips, const Vec3 &p_local, const LocoParams &p_params);

inline float advance_phase(float p_phase, float p_dt, float p_cycle_seconds) {
	if (!(p_cycle_seconds > 0.0f) || !(p_dt >= 0.0f) || !(p_phase >= 0.0f && p_phase < 1.0f) || p_dt > 1.0e6f) {
		return 0.0f;
	}
	float phase = p_phase + p_dt / p_cycle_seconds;
	if (!(phase < 1.0e9f)) {
		return 0.0f;
	}
	phase -= float(int64_t(phase));
	return phase >= 1.0f ? 0.0f : phase;
}

}

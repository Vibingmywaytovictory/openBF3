#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bf3::audio {

inline constexpr float kShakeLifeSeconds = 0.5f;
inline constexpr float kShakeWeightPerLife = 2.0f;
inline constexpr size_t kMaxShakeSources = 32;

struct CameraShake {
	float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
	uint32_t sources = 0;
};
static_assert(std::is_trivially_copyable_v<CameraShake>);

bool has_haptics(const PatchHaptics &p_haptics);

class HapticsFeed {
public:
	void note(const MixerFeedback &p_feedback);
	CameraShake step(float p_dt, float p_shake_factor);
	size_t sources() const { return count_; }
	uint64_t blocks() const { return blocks_; }
	float peak() const { return peak_; }

private:
	struct Entry {
		InstanceHandle source = kNoInstance;
		float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
		float life = 0.0f;
	};
	std::array<Entry, kMaxShakeSources> entries_{};
	size_t count_ = 0;
	uint64_t blocks_ = 0;
	uint64_t last_frame_ = 0;
	float peak_ = 0.0f;
};

}

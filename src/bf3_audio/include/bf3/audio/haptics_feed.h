#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"

#include <cstdint>
#include <type_traits>

namespace bf3::audio {

inline constexpr uint64_t kHapticsHoldFrames = 4 * kBlockFrames;

struct CameraShake {
	float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
	uint32_t sources = 0;
};
static_assert(std::is_trivially_copyable_v<CameraShake>);

bool has_haptics(const PatchHaptics &p_haptics);

class HapticsFeed {
public:
	void note(const MixerFeedback &p_feedback);
	CameraShake current(uint64_t p_now_frame) const;
	uint64_t blocks() const { return blocks_; }
	float peak() const { return peak_; }

private:
	uint64_t frame_ = 0;
	CameraShake sum_;
	uint64_t blocks_ = 0;
	float peak_ = 0.0f;
	bool any_ = false;
};

}

#include "bf3/audio/haptics_feed.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

bool has_haptics(const PatchHaptics &p_haptics) {
	return p_haptics.shake_pitch != 0.0f || p_haptics.shake_yaw != 0.0f || p_haptics.shake_roll != 0.0f;
}

void HapticsFeed::note(const MixerFeedback &p_feedback) {
	const PatchHaptics &h = p_feedback.haptics;
	if (!has_haptics(h) || !std::isfinite(h.shake_pitch) || !std::isfinite(h.shake_yaw) ||
			!std::isfinite(h.shake_roll)) {
		return;
	}
	if (!any_ || p_feedback.frame > frame_) {
		sum_ = CameraShake{};
		frame_ = p_feedback.frame;
		any_ = true;
		++blocks_;
	} else if (p_feedback.frame < frame_) {
		return;
	}
	sum_.pitch += h.shake_pitch;
	sum_.yaw += h.shake_yaw;
	sum_.roll += h.shake_roll;
	++sum_.sources;
	peak_ = std::max({ peak_, std::fabs(sum_.pitch), std::fabs(sum_.yaw), std::fabs(sum_.roll) });
}

CameraShake HapticsFeed::current(uint64_t p_now_frame) const {
	if (!any_ || p_now_frame > frame_ + kHapticsHoldFrames) {
		return CameraShake{};
	}
	return sum_;
}

}

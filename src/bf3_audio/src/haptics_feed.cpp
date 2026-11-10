#include "bf3/audio/haptics_feed.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace {

using Matrix = std::array<float, 9>;

Matrix multiply(const Matrix &p_a, const Matrix &p_b) {
	Matrix m{};
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) {
			m[r * 3 + c] = p_a[r * 3] * p_b[c] + p_a[r * 3 + 1] * p_b[3 + c] + p_a[r * 3 + 2] * p_b[6 + c];
		}
	}
	return m;
}

Matrix about_y(float p_a) {
	const float c = std::cos(p_a), s = std::sin(p_a);
	return Matrix{ c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c };
}

Matrix about_x(float p_a) {
	const float c = std::cos(p_a), s = std::sin(p_a);
	return Matrix{ 1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c };
}

Matrix about_z(float p_a) {
	const float c = std::cos(p_a), s = std::sin(p_a);
	return Matrix{ c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f };
}

}

bool has_haptics(const PatchHaptics &p_haptics) {
	return p_haptics.shake_pitch != 0.0f || p_haptics.shake_yaw != 0.0f || p_haptics.shake_roll != 0.0f;
}

void HapticsFeed::note(const MixerFeedback &p_feedback) {
	const PatchHaptics &h = p_feedback.haptics;
	if (!has_haptics(h) || !std::isfinite(h.shake_pitch) || !std::isfinite(h.shake_yaw) ||
			!std::isfinite(h.shake_roll)) {
		return;
	}
	if (blocks_ == 0 || p_feedback.frame != last_frame_) {
		++blocks_;
		last_frame_ = p_feedback.frame;
	}
	size_t at = 0;
	while (at < count_ && entries_[at].source != p_feedback.handle) {
		++at;
	}
	if (at == count_) {
		if (count_ == entries_.size()) {
			return;
		}
		++count_;
	}
	entries_[at] = Entry{ p_feedback.handle, h.shake_pitch, h.shake_yaw, h.shake_roll, kShakeLifeSeconds };
	peak_ = std::max({ peak_, std::fabs(h.shake_pitch), std::fabs(h.shake_yaw), std::fabs(h.shake_roll) });
}

CameraShake HapticsFeed::step(float p_dt, float p_shake_factor) {
	CameraShake out;
	Matrix m{ 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
	const float dt = std::isfinite(p_dt) ? std::max(0.0f, p_dt) : 0.0f;
	const float factor = std::isfinite(p_shake_factor) ? p_shake_factor : 0.0f;
	size_t kept = 0;
	for (size_t i = 0; i < count_; ++i) {
		Entry e = entries_[i];
		const float w = kShakeWeightPerLife * e.life * factor;
		m = multiply(multiply(multiply(m, about_y(e.yaw * w)), about_x(e.pitch * w)), about_z(e.roll * w));
		++out.sources;
		const float life_before = e.life;
		e.life -= dt;
		if (!(life_before < 0.0f)) {
			entries_[kept++] = e;
		}
	}
	count_ = kept;
	out.pitch = std::asin(std::clamp(-m[5], -1.0f, 1.0f));
	out.yaw = std::atan2(m[2], m[8]);
	out.roll = std::atan2(m[3], m[4]);
	return out;
}
}

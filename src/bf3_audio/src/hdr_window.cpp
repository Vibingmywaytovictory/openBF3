#include "bf3/audio/hdr_window.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace {

constexpr float kEpsilonDb = 1.0e-3f;
constexpr float kPrecullMarginDb = 3.0f;

float finite_or(float p_v, float p_fallback) {
	return std::isfinite(p_v) ? p_v : p_fallback;
}

}

void HdrWindow::configure(const core::audio::HdrSetting &p_setting, uint32_t p_mix_rate) {
	setting_ = p_setting;
	mix_rate_ = std::max<uint32_t>(p_mix_rate, 1);
	curve_y0_ = 1.0f;
	curve_y1_ = 0.0f;
	if (!setting_.top_release_curve.points.empty()) {
		curve_y0_ = setting_.top_release_curve.evaluate(0.0f);
		curve_y1_ = setting_.top_release_curve.evaluate(1.0f);
	}
	reset();
}

void HdrWindow::reset() {
	top_db_ = setting_.window_min_top_db;
	bottom_db_ = std::max(setting_.window_min_bottom_db, top_db_ - setting_.window_size_db);
	phase_ = Phase::Hold;
	ramp_from_db_ = ramp_to_db_ = top_db_;
	ramp_seconds_ = ramp_elapsed_ = 0.0f;
}

float HdrWindow::release_seconds(float p_drop_db) const {
	const float size = std::max(setting_.window_size_db, 1.0e-3f);
	const float f = std::clamp(finite_or(p_drop_db, 0.0f) / size, 0.0f, 1.0f);
	return setting_.top_min_release_s + (setting_.top_max_release_s - setting_.top_min_release_s) * f;
}

float HdrWindow::release_shape(float p_x) const {
	const float x = std::clamp(finite_or(p_x, 1.0f), 0.0f, 1.0f);
	const float span = curve_y0_ - curve_y1_;
	if (setting_.top_release_curve.points.empty() || !(span > 1.0e-4f) || !std::isfinite(span)) {
		return 1.0f - x;
	}
	const float y = setting_.top_release_curve.evaluate(x);
	return std::clamp(finite_or((y - curve_y1_) / span, 1.0f - x), 0.0f, 1.0f);
}

void HdrWindow::step(float p_loudest_db, uint32_t p_frames) {
	const float dt = float(p_frames) / float(mix_rate_);
	const float target = std::max(setting_.window_min_top_db, finite_or(p_loudest_db, setting_.window_min_top_db));
	if (target > top_db_ + kEpsilonDb) {

		if (setting_.top_attack_s <= 0.0f) {
			top_db_ = target;
			phase_ = Phase::Hold;
		} else {
			if (phase_ != Phase::Attack || target > ramp_to_db_ + kEpsilonDb) {
				phase_ = Phase::Attack;
				ramp_from_db_ = top_db_;
				ramp_to_db_ = target;
			} else {
				ramp_to_db_ = target;
			}
			const float slope = (ramp_to_db_ - ramp_from_db_) / setting_.top_attack_s;
			top_db_ = std::min(target, top_db_ + slope * dt);
			if (top_db_ >= target - kEpsilonDb) {
				top_db_ = target;
				phase_ = Phase::Hold;
			}
		}
	} else if (target < top_db_ - kEpsilonDb) {

		if (phase_ != Phase::Release || std::fabs(target - ramp_to_db_) > kEpsilonDb) {
			phase_ = Phase::Release;
			ramp_from_db_ = top_db_;
			ramp_to_db_ = target;
			ramp_seconds_ = release_seconds(top_db_ - target);
			ramp_elapsed_ = 0.0f;
		}
		ramp_elapsed_ += dt;
		const float x = ramp_seconds_ > 0.0f ? ramp_elapsed_ / ramp_seconds_ : 1.0f;
		if (x >= 1.0f) {
			top_db_ = ramp_to_db_;
			phase_ = Phase::Hold;
		} else {
			top_db_ = std::max(ramp_to_db_, ramp_to_db_ + (ramp_from_db_ - ramp_to_db_) * release_shape(x));
		}
	} else {
		top_db_ = std::max(top_db_, target);
		phase_ = Phase::Hold;
	}

	const float bottom_target = std::max(setting_.window_min_bottom_db, top_db_ - setting_.window_size_db);
	const float size = std::max(setting_.window_size_db, 1.0e-3f);
	if (bottom_target > bottom_db_) {
		bottom_db_ = setting_.bottom_attack_s <= 0.0f ? bottom_target : std::min(bottom_target, bottom_db_ + size / setting_.bottom_attack_s * dt);
	} else if (bottom_target < bottom_db_) {
		bottom_db_ = setting_.bottom_release_s <= 0.0f ? bottom_target : std::max(bottom_target, bottom_db_ - size / setting_.bottom_release_s * dt);
	}
}

float HdrWindow::gain_db(float p_loudness_db) const {
	const float l = std::min(finite_or(p_loudness_db, -200.0f), top_db_ + setting_.allowed_overshoot_db);
	return l - top_db_ - setting_.headroom_db;
}

bool HdrWindow::suspended(float p_loudness_db) const {
	return !(p_loudness_db >= bottom_db_);
}

float HdrWindow::precull_db() const {
	return bottom_db_ - kPrecullMarginDb;
}

}

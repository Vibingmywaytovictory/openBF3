#pragma once

#include "bf3/core/audio/hdr_setting.h"

#include <cstdint>

namespace bf3::audio {

class HdrWindow {
public:
	HdrWindow() = default;
	void configure(const core::audio::HdrSetting &p_setting, uint32_t p_mix_rate);
	void reset();

	void step(float p_loudest_db, uint32_t p_frames);
	float top_db() const { return top_db_; }
	float bottom_db() const { return bottom_db_; }
	float gain_db(float p_loudness_db) const;
	bool suspended(float p_loudness_db) const;
	float precull_db() const;
	const core::audio::HdrSetting &setting() const { return setting_; }

	float release_seconds(float p_drop_db) const;

	float release_shape(float p_x) const;

private:
	enum class Phase : uint8_t { Hold, Attack, Release };
	core::audio::HdrSetting setting_;
	uint32_t mix_rate_ = 48000;
	float top_db_ = 80.0f;
	float bottom_db_ = 35.0f;
	Phase phase_ = Phase::Hold;
	float ramp_from_db_ = 80.0f;
	float ramp_to_db_ = 80.0f;
	float ramp_seconds_ = 0.0f;
	float ramp_elapsed_ = 0.0f;
	float curve_y0_ = 1.0f, curve_y1_ = 0.0f;
};

}

#pragma once

#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace bf3::audio {

struct AudioTuning {

	float curve_x_metres = 1.0f;

	float min_distance_clamp = 1.0f;

	float lpb0_full_damping_hz = 2000.0f;
	float lpb0_open_hz = 20000.0f;
	float hf_damping_db_per_metre = 0.0f;
	float iir2_q = 0.70710678f;

	float lfe_fold_db = -6.0f;
	float window_top_db = 115.0f;
	float window_headroom_db = 6.0f;
	float master_gain_db = 0.0f;
	float pan_law_db = -3.0f;

	float step_crossfade_ms = 2.0f;
	float release_fade_ms = 5.0f;

	float max_voices = 128.0f;
	float control_block_frames = 512.0f;
	float schedule_lead_blocks = 2.0f;
};

struct AudioTuningKey {
	std::string_view section;
	std::string_view key;
	float AudioTuning::*field;
	float min;
	float max;
	std::string_view source;
};

std::span<const AudioTuningKey> audio_tuning_keys();

core::Result<AudioTuning> parse_audio_tuning(std::string_view p_text);

}

#pragma once

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <string_view>

namespace bf3::core::audio {

inline constexpr std::string_view kSoundStatesPartition = "sound/soundstates";

struct HdrSetting {
	float window_size_db = 45.0f;
	float window_min_top_db = 80.0f;
	float window_min_bottom_db = 35.0f;
	float top_attack_s = 0.05f;
	float top_min_release_s = 0.035f;
	float top_max_release_s = 0.85f;

	AudioCurve top_release_curve;
	float bottom_attack_s = 0.0f;
	float bottom_release_s = 0.025f;
	float headroom_db = 5.0f;
	float allowed_overshoot_db = 3.0f;
	float compress_factor = 0.5f;
	float discharge_factor = 3.35f;
	float max_allowed_energy = 110.0f;
};

AudioCurve default_hdr_release_curve();

HdrSetting retail_hdr_setting();

Status validate_hdr_setting(const HdrSetting &p_setting);

Result<HdrSetting> read_hdr_setting(const EbxResolver &p_db, gameplay::WorkBudget *p_budget = nullptr);

}

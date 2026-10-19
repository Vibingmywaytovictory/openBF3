#pragma once
// The audio system's HDR loudness window setting (docs/design/m2b.md §6.1, §2.6 #14; declared in W1.0, owner au).
// Read-only on the install, Godot-free. The retail data carries ONE HdrSetting (in the AudioSystemAsset's "Default"
// configuration) [verified: research/topics/gap-r1-audio-system.md §6.3]. The defaults below are those data values, so
// a test without an install runs the same window; read_hdr_setting replaces them with what the data holds.
// CompressFactor, DischargeFactor and MaxAllowedEnergy are read but have no known formula: not applied,
// measured = false (Q9).

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <array>
#include <vector>

namespace bf3::core::audio {

struct HdrSetting {
	float window_size_db = 45.0f; // WindowSize
	float window_min_top_db = 80.0f; // WindowMinTop
	float window_min_bottom_db = 35.0f; // WindowMinBottom
	float top_attack_s = 0.05f; // WindowTopAttackTime
	float top_min_release_s = 0.035f; // WindowTopMinReleaseTime
	float top_max_release_s = 0.85f; // WindowTopMaxReleaseTime
	// WindowTopReleaseTimeCurve: (x, y) points of the spline shaping the release between the two times (empty: linear).
	std::vector<std::array<float, 2>> top_release_curve;
	float bottom_attack_s = 0.0f; // WindowBottomAttackTime
	float bottom_release_s = 0.025f; // WindowBottomReleaseTime
	float headroom_db = 5.0f; // Headroom: applied to the amplitudes, not to the window top (§6.1)
	float allowed_overshoot_db = 3.0f; // AllowedOvershoot
	float compress_factor = 0.5f; // not applied (Q9)
	float discharge_factor = 3.35f; // not applied (Q9)
	float max_allowed_energy = 110.0f; // not applied (Q9)
};

// The install's HdrSetting (au): every read charged to the budget; an absent or malformed setting is an error.
Result<HdrSetting> read_hdr_setting(const EbxResolver &p_db, gameplay::WorkBudget *p_budget = nullptr);

} // namespace bf3::core::audio

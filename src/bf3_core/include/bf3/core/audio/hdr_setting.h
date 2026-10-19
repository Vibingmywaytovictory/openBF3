#pragma once
// The audio system's HDR loudness window setting (docs/design/m2b.md §6.1, §2.6 #14; declared in W1.0, owner au).
// Read-only on the install, Godot-free. The retail data carries ONE HdrSetting, in sound/soundstates (the
// SoundStateSettingsAsset; its "Default" state names it) [verified 2026-10-19 with bf3dump dump on MP_Subway:
// research/topics/gap-r1-audio-system.md §6.3 lists the same values]. The defaults below are those data values, so a
// test without an install runs the same window; read_hdr_setting replaces them with what the data holds.
// CompressFactor, DischargeFactor and MaxAllowedEnergy are read but have no known formula: not applied,
// measured = false (Q9).
//
// As built (au, docs/design/m2b-au.md): top_release_curve is the data's WindowTopReleaseTimeCurve, an AudioCurve
// (Hermite spline over (X, Y, K) points, sound_program.h) instead of the W1.0 sketch's (x, y) pairs, so the slopes
// are kept. Its five retail points run from (0.002, 0.976) down to (0.998, 0.008).

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <string_view>

namespace bf3::core::audio {

// The partition that holds the HdrSetting (searched by type, the first instance wins).
inline constexpr std::string_view kSoundStatesPartition = "sound/soundstates";

struct HdrSetting {
	float window_size_db = 45.0f; // WindowSize
	float window_min_top_db = 80.0f; // WindowMinTop
	float window_min_bottom_db = 35.0f; // WindowMinBottom
	float top_attack_s = 0.05f; // WindowTopAttackTime
	float top_min_release_s = 0.035f; // WindowTopMinReleaseTime
	float top_max_release_s = 0.85f; // WindowTopMaxReleaseTime
	// WindowTopReleaseTimeCurve: the release's shape (hdr_window.h says how it is applied; empty: linear).
	AudioCurve top_release_curve;
	float bottom_attack_s = 0.0f; // WindowBottomAttackTime
	float bottom_release_s = 0.025f; // WindowBottomReleaseTime
	float headroom_db = 5.0f; // Headroom: applied to the amplitudes, not to the window top (§6.1)
	float allowed_overshoot_db = 3.0f; // AllowedOvershoot (hdr_window.h: the attack's overshoot clamp, inferred)
	float compress_factor = 0.5f; // not applied (Q9)
	float discharge_factor = 3.35f; // not applied (Q9)
	float max_allowed_energy = 110.0f; // not applied (Q9)
};

// The retail WindowTopReleaseTimeCurve (the defaults' curve; read_hdr_setting replaces it with the data's).
AudioCurve default_hdr_release_curve();
// The defaults with the retail curve (HdrSetting{} leaves the curve empty).
HdrSetting retail_hdr_setting();

// Every field in range: sizes and levels in [0, 200] dB (WindowSize > 0), times in [0, 10] s, min release <= max
// release, the curve's points finite and sorted, at most kMaxCurvePoints. read_hdr_setting runs it on what it read.
Status validate_hdr_setting(const HdrSetting &p_setting);

// The install's HdrSetting (au): every instance examined and every curve point charged to the budget (a local
// budget of WorkBudget::kDefaultLimit when null); an absent, malformed or out-of-range setting is an error.
Result<HdrSetting> read_hdr_setting(const EbxResolver &p_db, gameplay::WorkBudget *p_budget = nullptr);

} // namespace bf3::core::audio

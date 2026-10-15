#pragma once
// Audio tuning: the [inferred] constants of the A1/A2 audio path, in one table with their sources (M2a audio;
// docs/design/m2a-audio.md §6.6). Plan D14: what the data does not state is tunable, never hard-coded in the
// evaluator. Loaded from res://config/audio_tuning.cfg (bf3-opensource/config/audio_tuning.cfg) by the Godot side
// and passed down; tests use the defaults. Every key has a range; a value outside it, an unknown key or a
// malformed line fails the parse with the line number (the file is ours, but a typo must not play silently).
//
// Format: "[section]" lines and "key = <float>" lines, '#' comments; the ConfigFile subset the project's other
// .cfg files use.

#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace bf3::audio {

struct AudioTuning {
	// [distance]
	float curve_x_metres = 1.0f; // AudioCurve X per metre of emitter distance [inferred: the M16A3 Cone curve
	                             // reaches -67 dB at X 179, the MultiCrossfader distance bands end at 300]
	float min_distance_clamp = 1.0f; // distances below MinDistance evaluate the curve at MinDistance [inferred]
	// [filters]
	float lpb0_full_damping_hz = 2000.0f; // LPB0 cutoff when HFDamping* reaches 1 [inferred; A3 measures]
	float lpb0_open_hz = 20000.0f; // LPB0 cutoff with no damping
	float hf_damping_db_per_metre = 0.0f; // extra HF loss per metre beyond MinDistance x HFDampingDistance [inferred]
	// [output]
	float lfe_fold_db = -6.0f; // the LFE bus folded into stereo until a 5.1 path exists (A3)
	float window_top_db = 115.0f; // fixed loudness window top until HDR (A3): 0 dBFS = window_top_db
	float window_headroom_db = 6.0f; // output gain = Loudness - window_top_db - headroom
	float master_gain_db = 0.0f;
	float pan_law_db = -3.0f; // equal-power pan centre level
	// [sampler]
	float step_crossfade_ms = 2.0f; // de-click ramp when a slot is taken over [inferred]
	float release_fade_ms = 5.0f; // LtInstantRelease fade
	// [mixer]
	float max_voices = 128.0f; // voice slots in the mixer core (integer)
	float control_block_frames = 512.0f; // control rate: one evaluation per mix block (Godot's fixed 512)
};

struct AudioTuningKey {
	std::string_view section;
	std::string_view key;
	float AudioTuning::*field;
	float min;
	float max;
	std::string_view source; // why this value: [inferred] reasoning or a research reference
};

// One entry per AudioTuning field, in declaration order.
std::span<const AudioTuningKey> audio_tuning_keys();

// Parses the .cfg text over the defaults (a key not in the text keeps its default).
core::Result<AudioTuning> parse_audio_tuning(std::string_view p_text);

} // namespace bf3::audio

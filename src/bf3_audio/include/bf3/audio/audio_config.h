#pragma once

#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace bf3::audio {

struct AudioTuning {

	// SOURCE: notes R2-D5 3 (the attenuation curve's X is metres).
	float curve_x_metres = 1.0f;

	// STAND-IN: below MinDistance the curve is read at MinDistance, where notes R2-D5 2 read it at max(0, d - MinDistance).
	// Settle: follow R2-D5.
	float min_distance_clamp = 1.0f;

	// STAND-IN: the three filter values below are hand-picked and not read; no output low-pass is built (notes R-D5 and
	// R3-D5 C give the 24000 / 500 Hz cutoff chain). Settle: build that chain.
	float lpb0_full_damping_hz = 2000.0f;
	float lpb0_open_hz = 20000.0f;
	float hf_damping_db_per_metre = 0.0f;
	// INFERRED: a Butterworth Q for the IIR2 high-pass, low-pass and shelf nodes, which have no Q port.
	// Settle: a note on those plugins' Q.
	float iir2_q = 0.70710678f;

	// STAND-IN: the LFE bus folds into the stereo sum 6 dB down when no master mix runs. Settle: the master patch's LFE route.
	float lfe_fold_db = -6.0f;
	// STAND-IN: the fixed loudness window (top 115 dB, 6 dB headroom, master 0 dB) used without a level HdrSetting.
	// Settle: the level's HdrSetting (notes R-D3).
	float window_top_db = 115.0f;
	float window_headroom_db = 6.0f;
	float master_gain_db = 0.0f;
	// SOURCE: notes R3-D5 test vectors (a centred source gives 0.7071 per side, -3 dB).
	float pan_law_db = -3.0f;

	// STAND-IN: the de-click on a slot takeover and at the end of a segment nothing follows, where notes Q12 4 joins with
	// no fade. Settle: a note on whether the voice player de-clicks.
	float step_crossfade_ms = 2.0f;
	// STAND-IN: the fade of a sampler cut, where notes R5-AU2 A3 and A6 stop with no sampler ramp and leave the player's
	// de-click open. Settle: a note on whether the voice player de-clicks a stop.
	float release_fade_ms = 5.0f;

	// STAND-IN: a cap on busy sampler slots; BF3 has no global voice cap (notes R4-D4) and limits the count by
	// AudioSettings.MaxAudibleSoundCount (notes R-D3 6). Settle: cull by that count as R-D3.
	float max_voices = 128.0f;
	float control_block_frames = 512.0f;
	// STAND-IN: a 2-block lead of a new sound ahead of the mix clock. Settle: the retail delay from a shot to its sound.
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

// Audio tuning table and its .cfg parser (audio_config.h).

#include "bf3/audio/audio_config.h"

#include <charconv>
#include <cmath>
#include <string>

namespace bf3::audio {

namespace {

constexpr AudioTuningKey kKeys[] = {
	{ "distance", "curve_x_metres", &AudioTuning::curve_x_metres, 0.01f, 100.0f, "[inferred] Cone curve X reads as metres (M16A3: -67 dB at 179)" },
	{ "distance", "min_distance_clamp", &AudioTuning::min_distance_clamp, 0.0f, 1.0f, "[inferred] 1 = evaluate the curve at MinDistance below it" },
	{ "filters", "lpb0_full_damping_hz", &AudioTuning::lpb0_full_damping_hz, 100.0f, 20000.0f, "[inferred] LPB0 cutoff at full HF damping; A3 measures" },
	{ "filters", "lpb0_open_hz", &AudioTuning::lpb0_open_hz, 1000.0f, 24000.0f, "[inferred] LPB0 cutoff with no damping" },
	{ "filters", "hf_damping_db_per_metre", &AudioTuning::hf_damping_db_per_metre, 0.0f, 1.0f, "[inferred] off in A2" },
	{ "filters", "iir2_q", &AudioTuning::iir2_q, 0.1f, 30.0f, "[inferred] Butterworth Q for HighPassIir2/LowPassIir2 (no Q port)" },
	{ "output", "lfe_fold_db", &AudioTuning::lfe_fold_db, -96.0f, 12.0f, "stereo fold of the LFE bus until A3" },
	{ "output", "window_top_db", &AudioTuning::window_top_db, 60.0f, 160.0f, "fixed loudness window until HDR (A3); M16A3 Loudness 110" },
	{ "output", "window_headroom_db", &AudioTuning::window_headroom_db, 0.0f, 48.0f, "headroom below 0 dBFS" },
	{ "output", "master_gain_db", &AudioTuning::master_gain_db, -96.0f, 24.0f, "" },
	{ "output", "pan_law_db", &AudioTuning::pan_law_db, -6.0f, 0.0f, "equal-power centre" },
	{ "sampler", "step_crossfade_ms", &AudioTuning::step_crossfade_ms, 0.0f, 50.0f, "[inferred] de-click ramp on slot takeover" },
	{ "sampler", "release_fade_ms", &AudioTuning::release_fade_ms, 0.0f, 200.0f, "[inferred] LtInstantRelease fade" },
	{ "mixer", "max_voices", &AudioTuning::max_voices, 1.0f, 1024.0f, "voice slots" },
	{ "mixer", "control_block_frames", &AudioTuning::control_block_frames, 512.0f, 512.0f, "the mixer core's render grid" },
	{ "mixer", "schedule_lead_blocks", &AudioTuning::schedule_lead_blocks, 1.0f, 16.0f, "[inferred] lead of a new burst ahead of the mix clock" },
};

std::string_view trim(std::string_view p_text) {
	while (!p_text.empty() && (p_text.front() == ' ' || p_text.front() == '\t' || p_text.front() == '\r')) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && (p_text.back() == ' ' || p_text.back() == '\t' || p_text.back() == '\r')) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

} // namespace

std::span<const AudioTuningKey> audio_tuning_keys() {
	return kKeys;
}

core::Result<AudioTuning> parse_audio_tuning(std::string_view p_text) {
	AudioTuning tuning;
	std::string_view section;
	size_t line_number = 0;
	while (!p_text.empty()) {
		++line_number;
		size_t end = p_text.find('\n');
		std::string_view line = p_text.substr(0, end);
		p_text.remove_prefix(end == std::string_view::npos ? p_text.size() : end + 1);
		size_t comment = line.find('#');
		if (comment != std::string_view::npos) {
			line = line.substr(0, comment);
		}
		line = trim(line);
		if (line.empty()) {
			continue;
		}
		std::string where = "audio tuning line " + std::to_string(line_number);
		if (line.front() == '[') {
			if (line.back() != ']') {
				return core::make_error(where + ": malformed section");
			}
			section = trim(line.substr(1, line.size() - 2));
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string_view::npos) {
			return core::make_error(where + ": expected key = value");
		}
		std::string_view key = trim(line.substr(0, eq));
		std::string_view value_text = trim(line.substr(eq + 1));
		const AudioTuningKey *found = nullptr;
		for (const AudioTuningKey &entry : kKeys) {
			if (entry.section == section && entry.key == key) {
				found = &entry;
				break;
			}
		}
		if (found == nullptr) {
			return core::make_error(where + ": unknown key [" + std::string(section) + "] " + std::string(key));
		}
		float value = 0.0f;
		auto [ptr, ec] = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
		if (ec != std::errc() || ptr != value_text.data() + value_text.size() || !std::isfinite(value)) {
			return core::make_error(where + ": not a number: " + std::string(value_text));
		}
		if (value < found->min || value > found->max) {
			return core::make_error(where + ": " + std::string(key) + " out of range [" + std::to_string(found->min) + ", " +
					std::to_string(found->max) + "]");
		}
		tuning.*(found->field) = value;
	}
	return tuning;
}

} // namespace bf3::audio

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::audio {

struct OnsetOptions {
	float window_ms = 2.0f;
	float floor_ms = 20.0f;
	float rise_db = 12.0f;
	float silence_dbfs = -90.0f;
	float min_gap_ms = 30.0f;
	float bed_sigma = 4.0f;
};

std::vector<uint64_t> detect_onsets(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, const OnsetOptions &p_options);

float rms_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames);
float peak_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames);

float band_energy_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, uint64_t p_first, uint64_t p_frames,
		float p_low_hz, float p_high_hz);

float predicted_highpass_band_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, uint64_t p_first,
		uint64_t p_frames, float p_low_hz, float p_high_hz, float p_cutoff_hz, float p_q);

struct BurstWindow {
	uint64_t first = 0;
	uint64_t end = 0;
};
struct OnsetSpacing {
	uint64_t count = 0;
	double mean_frames = 0.0;
	double max_deviation_frames = 0.0;
};
OnsetSpacing onset_spacing(std::span<const uint64_t> p_onsets, BurstWindow p_window, double p_expected_frames);

}

#pragma once
// Offline analysis of rendered audio for the gates (M2a audio; docs/design/m2a-audio.md §9, gates G7/G8): onsets,
// levels, band energy. Plain functions over float samples; no Godot, no allocation beyond the returned vectors.

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::audio {

struct OnsetOptions {
	float window_ms = 2.0f; // energy envelope window
	float rise_db = 12.0f; // envelope rise over the running floor that marks an onset
	float min_gap_ms = 30.0f; // refractory period
};

// Frames (into the mono sum of p_interleaved) where a transient starts.
std::vector<uint64_t> detect_onsets(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, const OnsetOptions &p_options);

// RMS level of the mono sum over [p_first, p_first + p_frames), dBFS (-144 for silence).
float rms_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames);
float peak_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames);

// Energy (dB) of the mono sum in [p_low_hz, p_high_hz], via a 4th-order band split of the RBJ biquads (dsp.h).
// The HPF check (gate G7) compares the 20-250 Hz band of the far render with the same render through a bypassed
// high-pass.
float band_energy_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, float p_low_hz, float p_high_hz);

struct OnsetSpacing {
	uint64_t count = 0;
	double mean_frames = 0.0;
	double max_deviation_frames = 0.0; // largest |gap - p_expected|
};
OnsetSpacing onset_spacing(std::span<const uint64_t> p_onsets, double p_expected_frames);

} // namespace bf3::audio

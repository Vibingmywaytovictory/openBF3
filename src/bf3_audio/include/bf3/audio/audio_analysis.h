#pragma once
// Offline analysis of rendered audio for the gates (M2a audio; docs/design/m2a-audio.md §9, gates G7/G8): onsets,
// levels, band energy. Plain functions over float samples; no Godot, no allocation beyond the returned vectors.

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::audio {

// Onset detector, single-frame resolution:
//   e[n] = mean of x^2 over the window_ms frames ending at n (a running sum: one envelope value per frame, no hop);
//   floor[n] = the mean of e over the floor_ms frames before the window (the mean, not the minimum: the 2 ms
//     envelope of a noisy tail swings by more than 12 dB, so a minimum floor fires inside the M16A3 corebass tails;
//     measured on the G7 render: 11-14 onsets per 10-round burst with the minimum, exactly 10 with the mean);
//   an onset is the first frame n where e[n] >= floor[n] x 10^(rise_db / 10) (and e[n] above silence_dbfs), refined
//   back to the first frame in [n - window, n] whose |x| reaches -20 dB of the largest |x| in [n - window, n + window];
//   then nothing for min_gap_ms.
// audio.dsp.unit tests it on a synthetic 75 ms click train over a sustained noise bed 20 dB down: exactly the clicks,
// each within 2 frames.
struct OnsetOptions {
	float window_ms = 2.0f; // 96 frames at 48 kHz
	float floor_ms = 20.0f;
	float rise_db = 12.0f;
	float silence_dbfs = -90.0f;
	float min_gap_ms = 30.0f; // refractory period
};

// Frames (into the mono sum of p_interleaved) where a transient starts.
std::vector<uint64_t> detect_onsets(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, const OnsetOptions &p_options);

// RMS level of the mono sum over [p_first, p_first + p_frames), dBFS (-144 for silence).
float rms_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames);
float peak_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames);

// Band energy (dB) of the mono sum over [p_first, p_first + p_frames): Hann-windowed 8192-point FFT frames with 50 %
// overlap, |X|^2 summed over the bins whose centre lies in [p_low_hz, p_high_hz). The HPF rules of G7/G8 use 20-100
// Hz.
float band_energy_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, uint64_t p_first, uint64_t p_frames,
		float p_low_hz, float p_high_hz);
// The same band energy predicted for p_interleaved after an RBJ high-pass at p_cutoff_hz / p_q: every bin's |X|^2 is
// weighted by the analytic |H(f)|^2 of the bilinear-transformed filter before summing (G7's voice-14 rule compares
// the filtered render's measured band energy with this prediction from the unfiltered one).
float predicted_highpass_band_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, uint64_t p_first,
		uint64_t p_frames, float p_low_hz, float p_high_hz, float p_cutoff_hz, float p_q);

// Onsets inside one burst window [first, end): the window comes from the check script (audio_check.h), never from
// the onsets themselves, so the gaps between bursts and the tails after a stop are not counted.
struct BurstWindow {
	uint64_t first = 0;
	uint64_t end = 0;
};
struct OnsetSpacing {
	uint64_t count = 0;
	double mean_frames = 0.0;
	double max_deviation_frames = 0.0; // largest |gap - p_expected| between consecutive onsets in the window
};
OnsetSpacing onset_spacing(std::span<const uint64_t> p_onsets, BurstWindow p_window, double p_expected_frames);

} // namespace bf3::audio

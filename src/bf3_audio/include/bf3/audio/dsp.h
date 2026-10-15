#pragma once
// DSP primitives of the A1/A2 mixer (M2a audio; docs/design/m2a-audio.md §6.4). Godot-free, allocation-free,
// real-time safe: every function here runs on the audio thread. Our own code from the textbook forms named below.
//
//   Biquad: the RBJ Audio EQ Cookbook (R. Bristow-Johnson) low-pass, high-pass, band-pass (constant 0 dB peak) and
//     peaking forms, transposed direct form II, coefficients recomputed once per control block and ramped across
//     the block to avoid zipper noise. HighPassIir2/LowPassIir2/BandPassIir2/PeakingIir2 (HI20/LI20/BI20/PI20)
//     map onto these [inferred: the plugin names and the node ports Frequency/Q/Bandwidth/Amplitude; gap audio §1.2-§1.3].
//   OnePoleLowPass: the output voices' LPB0 (HF damping), y += a (x - y) with a = 1 - exp(-2 pi fc / fs).
//   Resampler (Rsp0, lives with the sampler slot state in patch_sampler.cpp): cubic Hermite (Catmull-Rom) over a
//     4-sample history; ratio = wave rate / mix rate x pitch.
//   Pan: equal power over azimuth for the stereo fold of Pn21 (Pn21 construct params 45, 135, 2 on the M16A3 are
//     read but the 4-speaker layout is folded to 2 until A3).

#include <cstdint>

namespace bf3::audio {

float db_to_gain(float p_db);
float gain_to_db(float p_gain); // -inf guarded: returns -144 for 0

enum class BiquadShape : uint8_t { LowPass, HighPass, BandPass, Peaking, Bypass };

struct BiquadCoefficients {
	float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
	// p_frequency is clamped to [10 Hz, 0.45 p_rate], p_q to [0.1, 30]; p_gain_db only for Peaking. A HighPass at
	// p_frequency <= 0 is the identity {1, 0, 0, 0, 0} and Biquad::process passes samples through unchanged (bit
	// exact): the M16A3's voice-14 HPF (v46) is 0 Hz up to 17 m, and gate G7 pins that a 0 Hz high-pass leaves the
	// render bit-identical (its voice-6 HPF v49 is never 0 in third person: >= 80 Hz). HighPassIir2/LowPassIir2 have no Q port; they use the tuning
	// key iir2_q (0.7071, Butterworth [inferred]).
	static BiquadCoefficients design(BiquadShape p_shape, float p_rate, float p_frequency, float p_q, float p_gain_db);
};

struct Biquad {
	BiquadCoefficients c;
	float z1 = 0.0f, z2 = 0.0f;
	// In place over p_frames samples, interpolating from the current coefficients to p_target.
	void process(float *r_samples, uint32_t p_frames, const BiquadCoefficients &p_target);
	void reset() { z1 = z2 = 0.0f; }
};

struct OnePoleLowPass {
	float a = 1.0f, y = 0.0f;
	void set_cutoff(float p_rate, float p_frequency);
	void process(float *r_samples, uint32_t p_frames);
};

// Linear gain ramp from p_from to p_to over p_frames (de-zipper for every gain change).
void apply_gain_ramp(float *r_samples, uint32_t p_frames, float p_from, float p_to);

struct StereoGains {
	float left = 0.0f, right = 0.0f;
};
// p_azimuth in radians, 0 = ahead, positive = right; p_spread 0..1 (PanSize) narrows toward centre.
StereoGains equal_power_pan(float p_azimuth, float p_spread);

} // namespace bf3::audio

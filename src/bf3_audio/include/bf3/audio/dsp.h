#pragma once

#include <cstdint>

namespace bf3::audio {

float db_to_gain(float p_db);
float gain_to_db(float p_gain);

enum class BiquadShape : uint8_t { LowPass, HighPass, BandPass, Peaking, Bypass };

struct BiquadCoefficients {
	float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

	static BiquadCoefficients design(BiquadShape p_shape, float p_rate, float p_frequency, float p_q, float p_gain_db);
};

struct Biquad {
	BiquadCoefficients c;
	float z1 = 0.0f, z2 = 0.0f;

	void process(float *r_samples, uint32_t p_frames, const BiquadCoefficients &p_target);
	void reset() { z1 = z2 = 0.0f; }
};

struct OnePoleLowPass {
	float a = 1.0f, y = 0.0f;
	void set_cutoff(float p_rate, float p_frequency);
	void process(float *r_samples, uint32_t p_frames);
};

void apply_gain_ramp(float *r_samples, uint32_t p_frames, float p_from, float p_to);

struct StereoGains {
	float left = 0.0f, right = 0.0f;
};

StereoGains equal_power_pan(float p_azimuth, float p_spread);

}

#include "bf3/audio/dsp.h"

#include <algorithm>
#include <cmath>

namespace bf3::audio {

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool identity(const BiquadCoefficients &c) {
	return c.b0 == 1.0f && c.b1 == 0.0f && c.b2 == 0.0f && c.a1 == 0.0f && c.a2 == 0.0f;
}

bool same_coefficients(const BiquadCoefficients &p_a, const BiquadCoefficients &p_b) {
	return p_a.b0 == p_b.b0 && p_a.b1 == p_b.b1 && p_a.b2 == p_b.b2 && p_a.a1 == p_b.a1 && p_a.a2 == p_b.a2;
}
}

float db_to_gain(float p_db) {
	if (!(p_db > -144.0f)) {
		return 0.0f;
	}
	return std::pow(10.0f, std::min(p_db, 96.0f) / 20.0f);
}

float gain_to_db(float p_gain) {
	const float g = std::fabs(p_gain);
	if (!(g > 6.3e-8f)) {
		return -144.0f;
	}
	return 20.0f * std::log10(g);
}

BiquadCoefficients BiquadCoefficients::design(BiquadShape p_shape, float p_rate, float p_frequency, float p_q, float p_gain_db) {
	BiquadCoefficients c;
	if (p_shape == BiquadShape::Bypass || !(p_rate > 0.0f)) {
		return c;
	}
	if (p_shape == BiquadShape::HighPass && !(p_frequency > 0.0f)) {
		return c;
	}
	if (p_shape == BiquadShape::Peaking && p_gain_db == 0.0f) {
		return c;
	}
	const float f = std::clamp(std::isfinite(p_frequency) ? p_frequency : 10.0f, 10.0f, 0.45f * p_rate);
	const float q = std::clamp(std::isfinite(p_q) ? p_q : 0.70710678f, 0.1f, 30.0f);
	const double w0 = 2.0 * double(kPi) * double(f) / double(p_rate);
	const double cw = std::cos(w0);
	const double alpha = std::sin(w0) / (2.0 * double(q));
	double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
	switch (p_shape) {
		case BiquadShape::LowPass:
			b0 = (1.0 - cw) * 0.5;
			b1 = 1.0 - cw;
			b2 = b0;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cw;
			a2 = 1.0 - alpha;
			break;
		case BiquadShape::HighPass:
			b0 = (1.0 + cw) * 0.5;
			b1 = -(1.0 + cw);
			b2 = b0;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cw;
			a2 = 1.0 - alpha;
			break;
		case BiquadShape::BandPass:
			b0 = alpha;
			b1 = 0.0;
			b2 = -alpha;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cw;
			a2 = 1.0 - alpha;
			break;
		case BiquadShape::Peaking: {
			const double a = std::pow(10.0, double(std::clamp(p_gain_db, -96.0f, 48.0f)) / 40.0);
			b0 = 1.0 + alpha * a;
			b1 = -2.0 * cw;
			b2 = 1.0 - alpha * a;
			a0 = 1.0 + alpha / a;
			a1 = -2.0 * cw;
			a2 = 1.0 - alpha / a;
			break;
		}
		case BiquadShape::Bypass:
			break;
	}
	c.b0 = float(b0 / a0);
	c.b1 = float(b1 / a0);
	c.b2 = float(b2 / a0);
	c.a1 = float(a1 / a0);
	c.a2 = float(a2 / a0);
	return c;
}

void Biquad::process(float *r_samples, uint32_t p_frames, const BiquadCoefficients &p_target) {
	if (p_frames == 0) {
		return;
	}
	if (identity(c) && identity(p_target)) {
		z1 = z2 = 0.0f;
		return;
	}
	const bool ramp = c.b0 != p_target.b0 || c.b1 != p_target.b1 || c.b2 != p_target.b2 || c.a1 != p_target.a1 || c.a2 != p_target.a2;
	float b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
	float s1 = z1, s2 = z2;
	if (ramp) {
		const float inv = 1.0f / float(p_frames);
		const float db0 = p_target.b0 - c.b0, db1 = p_target.b1 - c.b1, db2 = p_target.b2 - c.b2;
		const float da1 = p_target.a1 - c.a1, da2 = p_target.a2 - c.a2;
		for (uint32_t i = 0; i < p_frames; ++i) {
			const float along = float(i + 1) * inv;
			b0 = c.b0 + db0 * along;
			b1 = c.b1 + db1 * along;
			b2 = c.b2 + db2 * along;
			a1 = c.a1 + da1 * along;
			a2 = c.a2 + da2 * along;
			const float x = r_samples[i];
			const float y = b0 * x + s1;
			s1 = b1 * x - a1 * y + s2;
			s2 = b2 * x - a2 * y;
			r_samples[i] = y;
		}
	} else {
		for (uint32_t i = 0; i < p_frames; ++i) {
			const float x = r_samples[i];
			const float y = b0 * x + s1;
			s1 = b1 * x - a1 * y + s2;
			s2 = b2 * x - a2 * y;
			r_samples[i] = y;
		}
	}
	if (!std::isfinite(s1) || !std::isfinite(s2)) {
		s1 = s2 = 0.0f;
	}
	z1 = s1;
	z2 = s2;
	c = p_target;
}

void process_biquad_pair(
		Biquad (&r_bq)[2], float *r_left, float *r_right, uint32_t p_frames, const BiquadCoefficients &p_target) {
	if (p_frames == 0 || !same_coefficients(r_bq[0].c, r_bq[1].c)) {
		r_bq[0].process(r_left, p_frames, p_target);
		r_bq[1].process(r_right, p_frames, p_target);
		return;
	}
	const BiquadCoefficients identity;
	if (same_coefficients(r_bq[0].c, identity) && same_coefficients(p_target, identity)) {
		r_bq[0].z1 = r_bq[0].z2 = r_bq[1].z1 = r_bq[1].z2 = 0.0f;
		return;
	}
	const BiquadCoefficients &c = r_bq[0].c;
	const bool ramp = !same_coefficients(c, p_target);
	float b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
	float l1 = r_bq[0].z1, l2 = r_bq[0].z2, q1 = r_bq[1].z1, q2 = r_bq[1].z2;
	if (ramp) {
		const float inv = 1.0f / float(p_frames);
		const float db0 = p_target.b0 - c.b0, db1 = p_target.b1 - c.b1, db2 = p_target.b2 - c.b2;
		const float da1 = p_target.a1 - c.a1, da2 = p_target.a2 - c.a2;
		for (uint32_t i = 0; i < p_frames; ++i) {
			const float along = float(i + 1) * inv;
			b0 = c.b0 + db0 * along;
			b1 = c.b1 + db1 * along;
			b2 = c.b2 + db2 * along;
			a1 = c.a1 + da1 * along;
			a2 = c.a2 + da2 * along;
			const float x = r_left[i], u = r_right[i];
			const float y = b0 * x + l1, w = b0 * u + q1;
			l1 = b1 * x - a1 * y + l2;
			q1 = b1 * u - a1 * w + q2;
			l2 = b2 * x - a2 * y;
			q2 = b2 * u - a2 * w;
			r_left[i] = y;
			r_right[i] = w;
		}
	} else {
		for (uint32_t i = 0; i < p_frames; ++i) {
			const float x = r_left[i], u = r_right[i];
			const float y = b0 * x + l1, w = b0 * u + q1;
			l1 = b1 * x - a1 * y + l2;
			q1 = b1 * u - a1 * w + q2;
			l2 = b2 * x - a2 * y;
			q2 = b2 * u - a2 * w;
			r_left[i] = y;
			r_right[i] = w;
		}
	}
	if (!std::isfinite(l1) || !std::isfinite(l2)) {
		l1 = l2 = 0.0f;
	}
	if (!std::isfinite(q1) || !std::isfinite(q2)) {
		q1 = q2 = 0.0f;
	}
	r_bq[0].z1 = l1;
	r_bq[0].z2 = l2;
	r_bq[1].z1 = q1;
	r_bq[1].z2 = q2;
	r_bq[0].c = p_target;
	r_bq[1].c = p_target;
}

void OnePoleLowPass::set_cutoff(float p_rate, float p_frequency) {
	if (!(p_rate > 0.0f) || !(p_frequency > 0.0f)) {
		a = 1.0f;
		return;
	}
	a = 1.0f - std::exp(-2.0f * kPi * std::min(p_frequency, 0.5f * p_rate) / p_rate);
}

void OnePoleLowPass::process(float *r_samples, uint32_t p_frames) {
	float s = y;
	for (uint32_t i = 0; i < p_frames; ++i) {
		s += a * (r_samples[i] - s);
		r_samples[i] = s;
	}
	y = std::isfinite(s) ? s : 0.0f;
}

void apply_gain_ramp(float *r_samples, uint32_t p_frames, float p_from, float p_to) {
	if (p_frames == 0) {
		return;
	}
	if (p_from == p_to) {
		if (p_to == 1.0f) {
			return;
		}
		for (uint32_t i = 0; i < p_frames; ++i) {
			r_samples[i] *= p_to;
		}
		return;
	}
	const float step = (p_to - p_from) / float(p_frames);
	float g = p_from;
	for (uint32_t i = 0; i < p_frames; ++i) {
		g += step;
		r_samples[i] *= g;
	}
}

StereoGains equal_power_pan(float p_azimuth, float p_spread) {
	const float az = std::isfinite(p_azimuth) ? p_azimuth : 0.0f;
	const float spread = std::clamp(std::isfinite(p_spread) ? p_spread : 0.0f, 0.0f, 1.0f);
	const float position = std::sin(az) * (1.0f - spread);
	const float theta = (position + 1.0f) * 0.25f * kPi;
	return StereoGains{ std::cos(theta), std::sin(theta) };
}

}

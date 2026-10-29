#include "bf3/audio/audio_analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace bf3::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kFftSize = 8192;

float mono_at(std::span<const float> p_x, uint32_t p_channels, uint64_t p_frame) {
	double s = 0.0;
	for (uint32_t c = 0; c < p_channels; ++c) {
		s += p_x[size_t(p_frame) * p_channels + c];
	}
	return float(s / double(p_channels));
}

uint64_t frames_of(std::span<const float> p_x, uint32_t p_channels) {
	return p_channels ? p_x.size() / p_channels : 0;
}

void fft(std::vector<std::complex<double>> &r_a) {
	const size_t n = r_a.size();
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(r_a[i], r_a[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double ang = -2.0 * kPi / double(len);
		const std::complex<double> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (size_t k = 0; k < len / 2; ++k) {
				const std::complex<double> u = r_a[i + k];
				const std::complex<double> v = r_a[i + k + len / 2] * w;
				r_a[i + k] = u + v;
				r_a[i + k + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

double highpass_power(double p_f, double p_rate, double p_cutoff, double p_q) {
	if (!(p_cutoff > 0.0)) {
		return 1.0;
	}
	const double fc = std::clamp(p_cutoff, 10.0, 0.45 * p_rate);
	const double q = std::clamp(p_q, 0.1, 30.0);
	const double w0 = 2.0 * kPi * fc / p_rate;
	const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
	const double b0 = (1.0 + cw) * 0.5, b1 = -(1.0 + cw), b2 = b0, a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
	const double w = 2.0 * kPi * p_f / p_rate;
	const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
	const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
	const std::complex<double> den = a0 + a1 * z1 + a2 * z2;
	return std::norm(num) / std::max(1e-300, std::norm(den));
}

template <class Weight>
float band_db(std::span<const float> p_x, uint32_t p_channels, uint32_t p_rate, uint64_t p_first, uint64_t p_frames, float p_low, float p_high,
		Weight &&p_weight) {
	const uint64_t total = frames_of(p_x, p_channels);
	if (p_channels == 0 || p_first >= total || p_frames == 0) {
		return -144.0f;
	}
	const uint64_t end = std::min(total, p_first + p_frames);
	std::vector<double> window(kFftSize);
	for (uint32_t i = 0; i < kFftSize; ++i) {
		window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(kFftSize));
	}
	std::vector<std::complex<double>> buf(kFftSize);
	double sum = 0.0;
	size_t frames = 0;
	const double bin_hz = double(p_rate) / double(kFftSize);
	uint64_t start = p_first;
	do {
		for (uint32_t i = 0; i < kFftSize; ++i) {
			const uint64_t f = start + i;
			buf[i] = std::complex<double>(f < end ? double(mono_at(p_x, p_channels, f)) * window[i] : 0.0, 0.0);
		}
		fft(buf);
		for (uint32_t k = 0; k <= kFftSize / 2; ++k) {
			const double hz = double(k) * bin_hz;
			if (hz >= p_low && hz < p_high) {
				sum += std::norm(buf[k]) * p_weight(hz);
			}
		}
		++frames;
		start += kFftSize / 2;
	} while (start + kFftSize <= end);
	const double mean = sum / double(std::max<size_t>(frames, 1));
	return mean > 0.0 ? float(10.0 * std::log10(mean)) : -144.0f;
}

}

std::vector<uint64_t> detect_onsets(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, const OnsetOptions &p_options) {
	std::vector<uint64_t> onsets;
	const uint64_t n = frames_of(p_interleaved, p_channels);
	if (n == 0 || p_rate == 0) {
		return onsets;
	}
	const uint64_t W = std::max<uint64_t>(1, uint64_t(std::llround(p_options.window_ms * 0.001 * p_rate)));
	const uint64_t F = std::max<uint64_t>(1, uint64_t(std::llround(p_options.floor_ms * 0.001 * p_rate)));
	const uint64_t gap = uint64_t(std::llround(p_options.min_gap_ms * 0.001 * p_rate));
	const double rise = std::pow(10.0, double(p_options.rise_db) / 10.0);
	const double silence = std::pow(10.0, double(p_options.silence_dbfs) / 10.0);
	std::vector<float> x(n);
	for (uint64_t i = 0; i < n; ++i) {
		x[i] = mono_at(p_interleaved, p_channels, i);
	}

	std::vector<double> e(n, 0.0);
	double run = 0.0;
	for (uint64_t i = 0; i < n; ++i) {
		run += double(x[i]) * x[i];
		if (i >= W) {
			run -= double(x[i - W]) * x[i - W];
		}
		e[i] = std::max(0.0, run) / double(W);
	}

	std::vector<double> ce(n + 1, 0.0);
	for (uint64_t i = 0; i < n; ++i) {
		ce[i + 1] = ce[i] + e[i];
	}
	uint64_t next_allowed = 0;
	for (uint64_t i = 0; i < n; ++i) {
		if (i < W + F || i < next_allowed) {
			continue;
		}
		const double floor = (ce[i - W + 1] - ce[i - W + 1 - F]) / double(F);
		if (e[i] > silence && e[i] >= std::max(floor, 1e-30) * rise) {
			const uint64_t lo = i >= W ? i - W : 0;
			const uint64_t hi = std::min(n - 1, i + W);
			float peak = 0.0f;
			for (uint64_t k = lo; k <= hi; ++k) {
				peak = std::max(peak, std::fabs(x[k]));
			}
			uint64_t onset = i;
			const float bed = float(double(p_options.bed_sigma) * std::sqrt(std::max(floor, 0.0)));
			const float threshold = std::max(0.1f * peak, std::min(bed, peak));
			for (uint64_t k = lo; k <= i; ++k) {
				if (std::fabs(x[k]) >= threshold) {
					onset = k;
					break;
				}
			}
			onsets.push_back(onset);
			next_allowed = onset + gap;
		}
	}
	return onsets;
}

float rms_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames) {
	const uint64_t total = frames_of(p_interleaved, p_channels);
	const uint64_t end = std::min(total, p_first + p_frames);
	if (p_first >= end) {
		return -144.0f;
	}
	double s = 0.0;
	for (uint64_t i = p_first; i < end; ++i) {
		const double v = mono_at(p_interleaved, p_channels, i);
		s += v * v;
	}
	const double ms = s / double(end - p_first);
	return ms > 0.0 ? float(10.0 * std::log10(ms)) : -144.0f;
}

float peak_db(std::span<const float> p_interleaved, uint32_t p_channels, uint64_t p_first, uint64_t p_frames) {
	const uint64_t total = frames_of(p_interleaved, p_channels);
	const uint64_t end = std::min(total, p_first + p_frames);
	float peak = 0.0f;
	for (uint64_t i = p_first; i < end; ++i) {
		peak = std::max(peak, std::fabs(mono_at(p_interleaved, p_channels, i)));
	}
	return peak > 0.0f ? 20.0f * std::log10(peak) : -144.0f;
}

float band_energy_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, uint64_t p_first, uint64_t p_frames, float p_low_hz,
		float p_high_hz) {
	return band_db(p_interleaved, p_channels, p_rate, p_first, p_frames, p_low_hz, p_high_hz, [](double) { return 1.0; });
}

float predicted_highpass_band_db(std::span<const float> p_interleaved, uint32_t p_channels, uint32_t p_rate, uint64_t p_first, uint64_t p_frames,
		float p_low_hz, float p_high_hz, float p_cutoff_hz, float p_q) {
	return band_db(p_interleaved, p_channels, p_rate, p_first, p_frames, p_low_hz, p_high_hz,
			[&](double p_hz) { return highpass_power(p_hz, double(p_rate), p_cutoff_hz, p_q); });
}

OnsetSpacing onset_spacing(std::span<const uint64_t> p_onsets, BurstWindow p_window, double p_expected_frames) {
	OnsetSpacing out;
	uint64_t prev = 0;
	bool have_prev = false;
	double gaps = 0.0;
	for (uint64_t o : p_onsets) {
		if (o < p_window.first || o >= p_window.end) {
			continue;
		}
		++out.count;
		if (have_prev) {
			const double g = double(o - prev);
			gaps += g;
			out.max_deviation_frames = std::max(out.max_deviation_frames, std::fabs(g - p_expected_frames));
		}
		prev = o;
		have_prev = true;
	}
	out.mean_frames = out.count > 1 ? gaps / double(out.count - 1) : 0.0;
	return out;
}

double integrated_loudness_lufs(std::span<const float> p_interleaved_stereo) {
	constexpr double kShelfB[3] = { 1.53512485958697, -2.69169618940638, 1.19839281085285 };
	constexpr double kShelfA[2] = { -1.69065929318241, 0.73248077421585 };
	constexpr double kHighB[3] = { 1.0, -2.0, 1.0 };
	constexpr double kHighA[2] = { -1.99004745483398, 0.99007225036621 };
	constexpr size_t kBlock = 19200;
	constexpr size_t kHop = 4800;
	const size_t frames = p_interleaved_stereo.size() / 2;
	if (frames < kBlock) {
		return kNoLoudness;
	}
	std::vector<double> power(frames, 0.0);
	for (size_t c = 0; c < 2; ++c) {
		double s1 = 0.0, s2 = 0.0, h1 = 0.0, h2 = 0.0;
		for (size_t i = 0; i < frames; ++i) {
			const double x = double(p_interleaved_stereo[i * 2 + c]);
			const double y = kShelfB[0] * x + s1;
			s1 = kShelfB[1] * x - kShelfA[0] * y + s2;
			s2 = kShelfB[2] * x - kShelfA[1] * y;
			const double z = kHighB[0] * y + h1;
			h1 = kHighB[1] * y - kHighA[0] * z + h2;
			h2 = kHighB[2] * y - kHighA[1] * z;
			power[i] += z * z;
		}
	}
	std::vector<double> prefix(frames + 1, 0.0);
	for (size_t i = 0; i < frames; ++i) {
		prefix[i + 1] = prefix[i] + power[i];
	}
	std::vector<double> blocks;
	for (size_t b = 0; b + kBlock <= frames; b += kHop) {
		blocks.push_back((prefix[b + kBlock] - prefix[b]) / double(kBlock));
	}
	const auto lufs = [](double p_mean_square) {
		return p_mean_square > 0.0 ? -0.691 + 10.0 * std::log10(p_mean_square) : kNoLoudness;
	};
	const auto gated_mean = [&](double p_gate) {
		double sum = 0.0;
		size_t n = 0;
		for (const double z : blocks) {
			if (lufs(z) > p_gate) {
				sum += z;
				++n;
			}
		}
		return n > 0 ? sum / double(n) : 0.0;
	};
	const double absolute = gated_mean(-70.0);
	if (!(absolute > 0.0)) {
		return kNoLoudness;
	}
	const double relative = gated_mean(lufs(absolute) - 10.0);
	return relative > 0.0 ? lufs(relative) : kNoLoudness;
}
}

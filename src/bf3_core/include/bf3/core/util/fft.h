#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

namespace bf3::core {

template <class T> class Fft {
public:
	explicit Fft(uint32_t p_size) {
		uint32_t bits = 0;
		while ((1u << bits) < p_size) {
			++bits;
		}
		size_ = 1u << bits;
		reverse_.resize(size_);
		for (uint32_t i = 0; i < size_; ++i) {
			uint32_t r = 0;
			for (uint32_t b = 0; b < bits; ++b) {
				r |= ((i >> b) & 1u) << (bits - 1 - b);
			}
			reverse_[i] = r;
		}
		cos_.resize(size_ / 2);
		sin_.resize(size_ / 2);
		for (uint32_t i = 0; i < size_ / 2; ++i) {
			const double a = 2.0 * std::numbers::pi * double(i) / double(size_);
			cos_[i] = T(std::cos(a));
			sin_[i] = T(std::sin(a));
		}
	}

	uint32_t size() const { return size_; }
	void forward(T *r_re, T *r_im) const { transform(r_re, r_im, T(-1)); }
	void inverse(T *r_re, T *r_im) const { transform(r_re, r_im, T(1)); }

private:
	void transform(T *r_re, T *r_im, T p_sign) const {
		for (uint32_t i = 0; i < size_; ++i) {
			const uint32_t j = reverse_[i];
			if (j > i) {
				std::swap(r_re[i], r_re[j]);
				std::swap(r_im[i], r_im[j]);
			}
		}
		for (uint32_t len = 2; len <= size_; len <<= 1) {
			const uint32_t half = len >> 1;
			const uint32_t stride = size_ / len;
			for (uint32_t start = 0; start < size_; start += len) {
				for (uint32_t k = 0; k < half; ++k) {
					const T wr = cos_[k * stride], wi = p_sign * sin_[k * stride];
					const uint32_t a = start + k, b = a + half;
					const T tr = r_re[b] * wr - r_im[b] * wi;
					const T ti = r_re[b] * wi + r_im[b] * wr;
					r_re[b] = r_re[a] - tr;
					r_im[b] = r_im[a] - ti;
					r_re[a] += tr;
					r_im[a] += ti;
				}
			}
		}
	}

	uint32_t size_ = 0;
	std::vector<uint32_t> reverse_;
	std::vector<T> cos_;
	std::vector<T> sin_;
};

}

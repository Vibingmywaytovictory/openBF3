#pragma once

#include <array>
#include <cstdint>

namespace bf3::ai {

class AIRandom {
public:
	static constexpr int kStateSize = 624;
	static constexpr int kOffset = 397;

	explicit AIRandom(int32_t p_seed);

	static int32_t seed_for(uint64_t p_match_seed, uint32_t p_slot);

	void init();
	uint32_t generate_numbers();

	uint32_t next() {
		if (index_ > kStateSize - 1) {
			generate_numbers();
		}
		uint32_t y = state_[size_t(index_++)];
		y ^= y >> 11;
		y ^= (y << 7) & 0x2B5B2500u;
		y ^= (y << 15) & 0xDB8B0000u;
		y ^= y >> 18;
		return y;
	}

	float next_float() { return float(double(next()) * 2.3283064e-10); }

	int32_t seed() const { return seed_; }

private:
	int32_t seed_ = 0;
	std::array<uint32_t, kStateSize> state_{};
	int index_ = 0;
};

}

#include "bf3/ai/main/ai_random.h"

#include "bf3/core/util/splitmix64.h"

namespace bf3::ai {

namespace {

constexpr uint32_t kMatrix[2] = { 0u, 0x9908B0DFu };

int32_t schrage_step(int32_t p_x, uint32_t p_multiplier) {
	const uint32_t x = uint32_t(p_x);
	uint32_t p = p_multiplier * x;
	uint32_t v = ((p_multiplier * (x & 0xFFFFu) >> 16) + p_multiplier * uint32_t(p_x >> 16)) >> 15;
	v += ((p >> 16) & 0x7FFFu) << 16;
	v += p & 0xFFFFu;
	int32_t y = int32_t(v - 0x7FFFFFFFu);
	if (y < 0) {
		y = int32_t(v);
	}
	return y;
}

}

AIRandom::AIRandom(int32_t p_seed) : seed_(p_seed), index_(0) {}

int32_t AIRandom::seed_for(uint64_t p_match_seed, uint32_t p_slot) {
	const uint64_t mixed = core::splitmix64(p_match_seed ^ (uint64_t(p_slot) * core::kSplitmix64Gamma));
	const int32_t seed = int32_t(mixed & 0x7FFFFFFFu);
	return seed != 0 ? seed : 1;
}

void AIRandom::init() {
	int32_t x = seed_ & 0x7FFFFFFF;
	for (int i = 0; i < kStateSize; ++i) {
		const int32_t y = schrage_step(x, 24112u);
		x = schrage_step(y, 26143u);
		state_[size_t(i)] = uint32_t(x);
	}
	index_ = kStateSize;
}

uint32_t AIRandom::generate_numbers() {
	index_ = 0;
	uint32_t y = 0;
	int i = 0;
	for (i = 0; i <= 226; ++i) {
		y = (state_[size_t(i)] & 0x80000000u) | (state_[size_t(i) + 1] & 0x7FFFFFFFu);
		state_[size_t(i)] = state_[size_t(i + kOffset)] ^ (y >> 1) ^ kMatrix[state_[size_t(i) + 1] & 1u];
	}
	for (i = 227; i <= 622; ++i) {
		y = (state_[size_t(i)] & 0x80000000u) | (state_[size_t(i) + 1] & 0x7FFFFFFFu);
		state_[size_t(i)] = state_[size_t(i + kOffset - kStateSize)] ^ (y >> 1) ^ kMatrix[state_[size_t(i) + 1] & 1u];
	}
	y = (state_[kStateSize - 1] & 0x80000000u) | (state_[0] & 0x7FFFFFFFu);
	state_[kStateSize - 1] = state_[kOffset - 1] ^ (y >> 1) ^ kMatrix[state_[0] & 1u];
	return state_[kStateSize - 1];
}

}

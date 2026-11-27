#pragma once

#include "bf3/core/util/splitmix64.h"

#include <cstdint>

namespace bf3::sim::weapons {

using core::splitmix64;

// STAND-IN: splitmix64 streams per shot for the draws BF3's sway generator does not make (pellets, fuses, delays).
// Settle: a note on how BF3 draws pellet directions and fuse variation.
constexpr uint64_t shot_seed(uint64_t p_match_seed, uint32_t p_player_slot, uint32_t p_shot_index) {
	return splitmix64(splitmix64(p_match_seed ^ (uint64_t(p_player_slot) << 32)) ^ uint64_t(p_shot_index));
}

enum class RngPurpose : uint8_t {
	SpreadRadius = 0,
	SpreadAngle = 1,
	PelletRadius = 2,
	PelletAngle = 3,
	RecoilHorizontal = 4,
	FuseVariation = 5,
	NearTargetDelay = 6,
};

constexpr uint64_t shot_random(uint64_t p_shot_seed, RngPurpose p_purpose, uint32_t p_lane = 0) {
	return splitmix64(p_shot_seed ^ splitmix64((uint64_t(uint8_t(p_purpose)) << 32) | uint64_t(p_lane)));
}

constexpr float unit_float(uint64_t p_bits) {
	return float(uint32_t(p_bits >> 40)) * (1.0f / 16777216.0f);
}

// SOURCE: notes Q2 randomness (31-bit Lehmer generator, x 24112 then x 26143 mod 2^31 - 1, a zero state taken as 1).
inline constexpr uint32_t kSwayRngModulus = 0x7FFFFFFFu;
inline constexpr uint32_t kSwayRngFirstFactor = 24112u;
inline constexpr uint32_t kSwayRngSecondFactor = 26143u;

constexpr uint32_t sway_rng_seeded(uint32_t p_seed) {
	return (p_seed == 0 ? 1u : p_seed) & kSwayRngModulus;
}

constexpr uint32_t sway_rng_next(uint32_t p_state) {
	const uint64_t s = p_state == 0 ? 1u : p_state;
	const uint64_t once = (s * kSwayRngFirstFactor) % kSwayRngModulus;
	return uint32_t((once * kSwayRngSecondFactor) % kSwayRngModulus);
}

// SOURCE: notes Q2 randomness (the unit draw from the state, in (0, 1] in steps of 2^-24).
constexpr float sway_rng_unit(uint32_t p_state) {
	return float(((p_state | 128u) >> 7) + 1u) * (1.0f / 16777216.0f);
}

constexpr float sway_uniform(uint32_t &r_state, double p_low, double p_high) {
	r_state = sway_rng_next(r_state);
	return float(p_low + (p_high - p_low) * double(sway_rng_unit(r_state)));
}
}

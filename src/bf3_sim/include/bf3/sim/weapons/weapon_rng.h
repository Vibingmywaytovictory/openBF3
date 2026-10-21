#pragma once

#include "bf3/core/util/splitmix64.h"

#include <cstdint>

namespace bf3::sim::weapons {

using core::splitmix64;

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
};

constexpr uint64_t shot_random(uint64_t p_shot_seed, RngPurpose p_purpose, uint32_t p_lane = 0) {
	return splitmix64(p_shot_seed ^ splitmix64((uint64_t(uint8_t(p_purpose)) << 32) | uint64_t(p_lane)));
}

constexpr float unit_float(uint64_t p_bits) {
	return float(uint32_t(p_bits >> 40)) * (1.0f / 16777216.0f);
}

}

#pragma once

#include "bf3/core/result.h"

#include <array>
#include <cstdint>

namespace bf3::core {
class EbxPartition;
}

namespace bf3::anim {

struct TemporalLodTier {
	float distance = 0.0f;
	float delta_time = 0.0f;
};

enum class FarPolicy : uint8_t {
	Freeze,
	LastTier,
};

struct TemporalLodTable {
	static constexpr size_t kTiers = 6;

	std::array<TemporalLodTier, kTiers> tiers{ { { 2.0f, 0.02f }, { 30.0f, 0.04f }, { 60.0f, 0.06f }, { 90.0f, 0.08f }, { 120.0f, 0.10f },
			{ 150.0f, 0.12f } } };
	float far_distance = 208.0f;
	FarPolicy far_policy = FarPolicy::Freeze;
	bool client_enable = true;
};

core::Result<TemporalLodTable> read_game_animation_settings(const core::EbxPartition &p_partition);

inline float temporal_lod_interval(const TemporalLodTable &p_table, float p_distance) {
	if (!p_table.client_enable || !(p_distance >= p_table.tiers[0].distance)) {
		return 0.0f;
	}
	if (p_distance > p_table.far_distance && p_table.far_policy == FarPolicy::Freeze) {
		return -1.0f;
	}
	float interval = p_table.tiers[0].delta_time;
	for (const TemporalLodTier &tier : p_table.tiers) {
		if (p_distance >= tier.distance) {
			interval = tier.delta_time;
		}
	}
	return interval;
}

inline int temporal_lod_tier(const TemporalLodTable &p_table, float p_distance) {
	if (!(p_distance >= p_table.tiers[0].distance)) {
		return -1;
	}
	if (p_distance > p_table.far_distance) {
		return int(TemporalLodTable::kTiers);
	}
	int tier = 0;
	for (size_t i = 0; i < TemporalLodTable::kTiers; ++i) {
		if (p_distance >= p_table.tiers[i].distance) {
			tier = int(i);
		}
	}
	return tier;
}

}

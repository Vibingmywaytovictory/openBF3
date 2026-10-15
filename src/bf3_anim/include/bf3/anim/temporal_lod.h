#pragma once
// Temporal LOD for animated characters from the GameAnimationSettings in the install (S3; docs/design/s3.md §5.7;
// research/topics/03-animation.md C17).
//
// Retail values [verified 2026-10-15, superbundle Win32/default_settings_win32, partition default/settings_win32,
// instance GameAnimationSettings 88caa308-9297-1820-2e34-1a5d322999c6]:
//   tier      First  Second  Third  Fourth  Fifth  Sixth     TemporalLoddingFarDistance 208
//   distance  2      30      60     90      120    150 m     ClientEnable true, ServerEnable true
//   delta     0.02   0.04    0.06   0.08    0.10   0.12 s
// The data names only distances and delta times. Their meaning here is an INFERENCE (s3.md §5.7): a character at
// camera distance d >= tiers[i].distance updates at most every tiers[i].delta_time seconds. Nearer than the
// first tier it updates every frame. Beyond far_distance, far_policy says what happens. Between updates the
// previous pose is kept and only the root transform follows the simulation.

#include "bf3/core/result.h"

#include <array>
#include <cstdint>

namespace bf3::core {
class EbxPartition;
}

namespace bf3::anim {

struct TemporalLodTier {
	float distance = 0.0f; // metres from the camera
	float delta_time = 0.0f; // seconds between pose updates at and beyond the distance
};

enum class FarPolicy : uint8_t {
	Freeze, // beyond far_distance: no pose updates (the root still moves)
	LastTier, // beyond far_distance: keep the last tier's delta time
};

struct TemporalLodTable {
	static constexpr size_t kTiers = 6;
	// Defaults are the retail values above, so a table that was never read still behaves like the game data.
	std::array<TemporalLodTier, kTiers> tiers{ { { 2.0f, 0.02f }, { 30.0f, 0.04f }, { 60.0f, 0.06f }, { 90.0f, 0.08f }, { 120.0f, 0.10f },
			{ 150.0f, 0.12f } } };
	float far_distance = 208.0f;
	FarPolicy far_policy = FarPolicy::Freeze;
	bool client_enable = true; // GameAnimationSettings.ClientEnable
};

// Reads the first GameAnimationSettings instance of p_partition (TemporalLodding<Ordinal>Distance /
// <Ordinal>DeltaTime, TemporalLoddingFarDistance, ClientEnable). Fails when it is absent. Also fails when a
// value is not finite or is negative, when the distances do not increase, or when far_distance is below the
// last tier.
core::Result<TemporalLodTable> read_game_animation_settings(const core::EbxPartition &p_partition);

// Pose update interval in seconds at camera distance p_distance: 0 means every frame; a negative value means
// never (FarPolicy::Freeze beyond far_distance). A table with client_enable false returns 0 everywhere.
inline float temporal_lod_interval(const TemporalLodTable &p_table, float p_distance) {
	if (!p_table.client_enable || !(p_distance >= p_table.tiers[0].distance)) {
		return 0.0f; // also NaN distances: update, never freeze on bad input
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

// Tier index for reports: -1 nearer than the first tier, 0..5, or 6 beyond far_distance.
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

} // namespace bf3::anim

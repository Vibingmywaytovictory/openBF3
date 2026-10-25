#pragma once

#include "bf3/host/presentation_events.h"
#include "bf3/present/presented.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bf3::host {

inline constexpr size_t kRoundStatsSlots = 64;
inline constexpr size_t kRoundStatsDefs = 256;
inline constexpr uint8_t kRoundStatsNoSlot = 0xFF;

struct RoundStats {
	uint8_t own_slot = kRoundStatsNoSlot;
	std::array<std::array<uint16_t, kRoundStatsSlots>, kRoundStatsSlots> kills{};
	std::array<std::array<uint16_t, kRoundStatsSlots>, kRoundStatsSlots> nemesis{};
	std::array<uint16_t, kRoundStatsDefs> own_weapon_kills{};
	uint32_t own_kills = 0;
	uint32_t own_deaths = 0;
	uint32_t own_headshots = 0;
	uint32_t own_suicides = 0;
	uint32_t streak = 0;
	uint32_t longest_streak = 0;
	uint32_t longest_nemesis_streak = 0;
	float longest_headshot_m = 0.0f;
	uint64_t deaths_seen = 0;
	uint64_t bad_events = 0;
};
static_assert(std::is_trivially_copyable_v<RoundStats>);

struct RoundStatsInput {
	uint8_t own_slot = kRoundStatsNoSlot;
	sim::Vec3 own_position{};
	std::span<const present::PresentedSoldier> others;
	std::span<const int8_t> teams;
};

void round_stats_reset(RoundStats &r_stats, uint8_t p_own_slot);

void round_stats_add(RoundStats &r_stats, const RoundStatsInput &p_input, std::span<const PresentEvent> p_events);

uint16_t round_stats_kills(const RoundStats &p_stats, uint8_t p_killer, uint8_t p_victim);

uint16_t round_stats_nemesis(const RoundStats &p_stats, uint8_t p_killer, uint8_t p_victim);

struct RoundBestWeapon {
	uint16_t def = 0xFFFF;
	uint16_t kills = 0;
};
RoundBestWeapon round_stats_best_weapon(const RoundStats &p_stats);

float round_stats_per_minute(double p_count, double p_seconds);

}

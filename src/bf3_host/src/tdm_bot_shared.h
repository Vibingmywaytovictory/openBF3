#pragma once

#include "bf3/host/bot_fill.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_common.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::host {

class TdmStrategy;

namespace tdm_bot {

inline constexpr int kHeatCells = 64;

struct RecentShot {
	uint8_t shooter = 0;
	sim::Tick tick = 0;
	sim::Vec3 origin{};
	sim::Vec3 direction{};
};

}

struct TdmShared {
	TdmBrainHost host;
	std::shared_ptr<const TdmBotConfig> config;
	TdmBotStats *stats = nullptr;
	TdmBotObserver *observer = nullptr;
	std::unique_ptr<BotNav> nav;
	sim::bots::WalkerParams walker;
	std::shared_ptr<TdmStrategy> strategy;

	sim::Tick scan_tick = UINT32_MAX;
	std::vector<uint8_t> teams, alive, was_alive, firing;
	std::vector<sim::Tick> fired_tick;
	std::vector<uint32_t> shot_cursor;
	std::vector<tdm_bot::RecentShot> ring;
	std::vector<sim::bots::HeardShot> heard;

	std::array<float, tdm_bot::kHeatCells * tdm_bot::kHeatCells> heat{};
	std::array<sim::Tick, tdm_bot::kHeatCells * tdm_bot::kHeatCells> heat_tick{};

	sim::Tick time_tick = UINT32_MAX;
	uint64_t perception_ns = 0, brains_ns = 0;

	template <typename T>
	void count(std::atomic<T> TdmBotStats::*p_field, T p_amount = 1) {
		if (stats != nullptr) {
			(stats->*p_field).fetch_add(p_amount, std::memory_order_relaxed);
		}
	}
	float eye_of(sim::Pose p_pose) const {
		const size_t i = size_t(p_pose) < 3 ? size_t(p_pose) : 0;
		return host.eye_heights[i] > 0.0f ? host.eye_heights[i] : 1.6f;
	}
	sim::Vec3 eye_point(const sim::SoldierState &p_state) const { return tdm_bot::add(p_state.position, 0.0f, eye_of(p_state.pose), 0.0f); }
	sim::Vec3 chest_point(const sim::SoldierState &p_state) const { return sim::bots::perception_chest(config->perception, p_state, host.eye_heights); }

	int heat_cell(const sim::Vec3 &p_at) const;
	float heat_at(int p_cell, sim::Tick p_tick);
	void add_heat(const sim::Vec3 &p_at, float p_amount, sim::Tick p_tick);
	sim::Vec3 heat_centre(int p_cell) const;

	void begin_tick(sim::Tick p_tick);
};

}

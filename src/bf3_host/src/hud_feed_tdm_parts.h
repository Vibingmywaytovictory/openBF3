#pragma once

#include "bf3/host/hud_feed.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bf3::host::tdm_feed {

inline constexpr sim::Tick kMaxAheadTicks = sim::Tick(3600) * sim::kSimHz;

struct Rank {
	uint8_t slot;
	int32_t score;
	int16_t kills;
};

struct Frame {
	const HudFeedInput &input;
	const HudStatic &statics;
	HudTdmFeedState &state;
	core::ui::HudModel &model;

	bool match = false;
	net::MatchPhase phase = net::MatchPhase::Loading;
	sim::Tick now_tick = 0;
	double now_s = 0;
	uint8_t own_slot = core::ui::kHudNoPlayer;
	uint8_t own_team = 0;
	uint8_t my_team = 1;
	uint8_t enemy_team = 2;
	uint8_t winner = 0;
	bool round_over = false;
	int32_t team1_kills = 0;
	int32_t team2_kills = 0;
	int32_t kill_limit = 0;
	std::array<bool, core::ui::kHudMaxPlayers> present{};
	std::array<Rank, core::ui::kHudMaxPlayers> ranked{};
	size_t ranked_count = 0;
	bool card_up = false;
	bool card_pending = false;
	std::array<int8_t, core::ui::kHudMaxPlayers> teams{};
};

float seconds_until(sim::Tick p_now, sim::Tick p_until, uint64_t &r_bad);

void feed_scoreboard(Frame &r_frame);
void feed_kill_card(Frame &r_frame, std::span<const PresentEvent> p_drained);
void feed_spawn(Frame &r_frame);
void feed_round_end(Frame &r_frame);
void feed_round_stats(Frame &r_frame, std::span<const PresentEvent> p_drained);

}

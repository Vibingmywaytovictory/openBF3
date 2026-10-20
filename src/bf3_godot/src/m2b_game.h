#pragma once

#include "m2a_game.h"

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <span>
#include <string>

namespace bf3::godot_side {

inline constexpr uint32_t kM2bPlayableDefaultBots = 31;

inline constexpr uint32_t kM2bSpawnRequestRetryTicks = 30;

struct M2bOptions {
	bool deploy_screen = true;
	int kill_limit = 0;
	float preround_s = -1.0f;
	uint32_t bots = kM2bPlayableDefaultBots;
	std::string tdm_tuning_cfg;
	std::string bot_tuning_cfg;
};

struct M2bLoadInputs {
	M2aLoadInputs m2a;
	M2bOptions options;
};

struct M2bFrame {
	const M2aFrame *m2a = nullptr;
	bool match_valid = false;
	const net::MatchState *match = nullptr;
	std::span<const net::PlayerRow> players;
	sim::Tick server_tick = 0;
	double now_s = 0.0;
	bool own_alive = false;
	bool own_unspawned = false;
	bool spawn_screen = false;
	bool death_view = false;
};

}

#include "m2b_check.h"

#include "bf3/core/result.h"

#include <vector>

namespace bf3::godot_side {

inline constexpr uint32_t kM2bMaxBots = 31;
inline constexpr float kM2bMaxPreroundSeconds = 600.0f;
inline constexpr int kM2bMaxKillLimit = 100000;

struct M2bArgText {
	bool has_kill_limit = false, has_preround = false, has_bots = false, has_deploy_screen = false;
	std::string kill_limit, preround, bots, deploy_screen;
};

core::Status parse_m2b_options(const M2bArgText &p_args, M2bOptions &r_options);

struct M2bScreenInput {
	bool match_valid = false;
	net::MatchPhase phase = net::MatchPhase::Loading;
	bool own_alive = false;
	bool own_unspawned = false;
	bool kill_card_up = false;
};
bool m2b_spawn_screen_up(const M2bScreenInput &p_input);

bool m2b_can_deploy(bool p_match_valid, const net::MatchState &p_match, sim::Tick p_server_tick, sim::Tick p_own_respawn_tick);

struct M2bDeploy {
	bool armed = false;
	uint8_t kit = 0;
	bool sent = false;
	sim::Tick sent_tick = 0;
	uint64_t requests = 0, resends = 0;
};

bool m2b_deploy_step(M2bDeploy &r_deploy, bool p_screen_up, bool p_can_deploy, sim::Tick p_server_tick);

void m2b_deploy_spawned(M2bDeploy &r_deploy);

bool m2b_scoreboard_shown(bool p_held, bool p_match_valid, net::MatchPhase p_phase);

bool m2b_death_had_screen_time(double p_death_t, double p_now_t, double p_round_over_t, double p_need_s);

struct M2bSeekSpot {
	sim::Vec3 position{};
	float yaw = 0.0f;
};
M2bSeekSpot m2b_seek_spot(const sim::Vec3 &p_enemy, float p_enemy_yaw, float p_metres);

struct M2bPlayTally {
	double seconds = 0;
	bool windowed = false;

	bool match_seen = false;
	bool in_round_seen = false;
	bool join_screen_seen = false;
	uint32_t own_deaths = 0;
	uint32_t deaths_checked = 0;
	uint32_t screens_after_death = 0;
	uint32_t deploys = 0;
	uint32_t deploy_kit_mismatch = 0;
	uint32_t kits_deployed_mask = 0;
	uint64_t spawn_requests = 0, spawn_request_resends = 0;
	bool round_over_seen = false;
	uint64_t deaths_received = 0;

	uint64_t feed_lines = 0;
	uint32_t scoreboard_samples = 0, scoreboard_mismatch = 0;
	uint32_t kill_cards_checked = 0, kill_cards_bad = 0;
	uint32_t spawn_screen_model_shown = 0;
	bool round_end_shown = false;
	uint64_t missing_lookups = 0;

	uint64_t death_views = 0;
	uint64_t death_view_bad_inputs = 0;
};

enum class M2bRowState : uint8_t { Pass = 0, Fail = 1, Skip = 2 };
struct M2bPlayRow {
	std::string name;
	double value = 0.0;
	std::string rule;
	M2bRowState state = M2bRowState::Skip;
	M2bTrack track = M2bTrack::Gx;
	std::string skip_reason;
};

std::vector<M2bPlayRow> m2b_play_rows(const M2bPlayTally &p_tally, bool p_have_ui, bool p_have_cam);

}

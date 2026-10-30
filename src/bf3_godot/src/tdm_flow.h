#pragma once

#include "bf3/core/result.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <span>
#include <string>

namespace bf3::godot_side {

struct GameFrame;

inline constexpr uint32_t kTdmDefaultBots = 31;

inline constexpr uint32_t kSpawnRequestRetryTicks = 30;

struct TdmOptions {
	bool deploy_screen = true;
	int kill_limit = 0;
	float preround_s = -1.0f;
	int min_players = -1;
	uint32_t bots = kTdmDefaultBots;
	std::string tdm_tuning_cfg;
	std::string bot_tuning_cfg;
};

struct TdmPreview {
	bool on = false;
	TdmOptions options;
	bool have_match = false;
	net::MatchState last;
};

struct TdmFrame {
	const GameFrame *game = nullptr;
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

inline constexpr uint32_t kTdmMaxBots = 31;
inline constexpr float kTdmMaxPreroundSeconds = 600.0f;
inline constexpr int kTdmMaxKillLimit = 100000;
inline constexpr int kTdmMaxMinPlayers = 32;

struct TdmArgText {
	bool has_kill_limit = false, has_preround = false, has_bots = false, has_deploy_screen = false, has_min_players = false;
	std::string kill_limit, preround, bots, deploy_screen, min_players;
};

core::Status parse_tdm_options(const TdmArgText &p_args, TdmOptions &r_options);
TdmArgText tdm_arg_text();
std::string config_path(const char *p_res);

struct TdmScreenInput {
	bool match_valid = false;
	net::MatchPhase phase = net::MatchPhase::Loading;
	bool own_alive = false;
	bool own_unspawned = false;
	bool kill_card_up = false;
};
bool tdm_spawn_screen_up(const TdmScreenInput &p_input);

bool tdm_can_deploy(bool p_match_valid, const net::MatchState &p_match, sim::Tick p_server_tick, sim::Tick p_own_respawn_tick);

struct TdmDeploy {
	bool armed = false;
	uint8_t kit = 0;
	bool sent = false;
	sim::Tick sent_tick = 0;
	uint64_t requests = 0, resends = 0;
};

bool tdm_deploy_step(TdmDeploy &r_deploy, bool p_screen_up, bool p_can_deploy, sim::Tick p_server_tick);

void tdm_deploy_spawned(TdmDeploy &r_deploy);

}

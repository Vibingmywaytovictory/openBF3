#include "tdm_flow.h"

#include <charconv>
#include <cmath>
#include <system_error>

namespace bf3::godot_side {

namespace {

bool parse_int(const std::string &p_text, long long &r_value) {
	if (p_text.empty() || p_text.size() > 18) {
		return false;
	}
	const char *end = p_text.data() + p_text.size();
	const std::from_chars_result r = std::from_chars(p_text.data(), end, r_value);
	return r.ec == std::errc() && r.ptr == end;
}

bool parse_seconds(const std::string &p_text, double &r_value) {
	if (p_text.empty() || p_text.size() > 24) {
		return false;
	}
	const char *end = p_text.data() + p_text.size();
	const std::from_chars_result r = std::from_chars(p_text.data(), end, r_value, std::chars_format::fixed);
	return r.ec == std::errc() && r.ptr == end && std::isfinite(r_value);
}

}

core::Status parse_tdm_options(const TdmArgText &p_args, TdmOptions &r_options) {
	std::string error;
	const auto bad = [&](const std::string &p_line) {
		if (error.empty()) {
			error = p_line;
		}
	};
	if (p_args.has_kill_limit) {
		long long v = 0;
		if (!parse_int(p_args.kill_limit, v) || v < 0 || v > kTdmMaxKillLimit) {
			bad("--kill-limit=" + p_args.kill_limit + ": not 0.." + std::to_string(kTdmMaxKillLimit) + " (0: the data's 100)");
		} else {
			r_options.kill_limit = int(v);
		}
	}
	if (p_args.has_preround) {
		double v = 0;
		if (!parse_seconds(p_args.preround, v) || v < 0.0 || v > double(kTdmMaxPreroundSeconds)) {
			bad("--preround=" + p_args.preround + ": not 0.." + std::to_string(int(kTdmMaxPreroundSeconds)) + " seconds");
		} else {
			r_options.preround_s = float(v);
		}
	}
	if (p_args.has_min_players) {
		long long v = 0;
		if (!parse_int(p_args.min_players, v) || v < 0 || v > kTdmMaxMinPlayers) {
			bad("--min-players=" + p_args.min_players + ": not 0.." + std::to_string(kTdmMaxMinPlayers) + " players");
		} else {
			r_options.min_players = int(v);
		}
	}
	if (p_args.has_bots) {
		long long v = 0;
		if (!parse_int(p_args.bots, v) || v < 0 || v > kTdmMaxBots) {
			bad("--bots=" + p_args.bots + ": not 0.." + std::to_string(kTdmMaxBots) + " (16 v 16 with the player)");
		} else {
			r_options.bots = uint32_t(v);
		}
	}
	if (p_args.has_deploy_screen) {
		if (p_args.deploy_screen == "1" || p_args.deploy_screen.empty()) {
			r_options.deploy_screen = true;
		} else if (p_args.deploy_screen == "0") {
			r_options.deploy_screen = false;
		} else {
			bad("--deploy-screen=" + p_args.deploy_screen + ": not 0 or 1");
		}
	}
	if (!error.empty()) {
		return core::make_error(error);
	}
	return {};
}

bool tdm_spawn_screen_up(const TdmScreenInput &p_input) {
	if (!p_input.match_valid || p_input.kill_card_up) {
		return false;
	}
	if (p_input.phase != net::MatchPhase::PreRound && p_input.phase != net::MatchPhase::InRound) {
		return false;
	}
	return !p_input.own_alive || p_input.own_unspawned;
}

bool tdm_can_deploy(bool p_match_valid, const net::MatchState &p_match, sim::Tick p_server_tick, sim::Tick p_own_respawn_tick) {
	if (!p_match_valid || (p_match.phase != net::MatchPhase::PreRound && p_match.phase != net::MatchPhase::InRound)) {
		return false;
	}
	return p_server_tick >= p_own_respawn_tick;
}

bool tdm_deploy_step(TdmDeploy &r_deploy, bool p_screen_up, bool p_can_deploy, sim::Tick p_server_tick) {
	if (!r_deploy.armed || !p_screen_up || !p_can_deploy) {
		return false;
	}
	if (r_deploy.sent && p_server_tick >= r_deploy.sent_tick && p_server_tick - r_deploy.sent_tick < kSpawnRequestRetryTicks) {
		return false;
	}
	if (r_deploy.sent) {
		++r_deploy.resends;
	}
	r_deploy.sent = true;
	r_deploy.sent_tick = p_server_tick;
	++r_deploy.requests;
	return true;
}

void tdm_deploy_spawned(TdmDeploy &r_deploy) {
	r_deploy.armed = false;
	r_deploy.sent = false;
	r_deploy.sent_tick = 0;
}

bool tdm_scoreboard_shown(bool p_held, bool p_match_valid, net::MatchPhase p_phase) {
	return p_held && !(p_match_valid && p_phase == net::MatchPhase::RoundOver);
}

}

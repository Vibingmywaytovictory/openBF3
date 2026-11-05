#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace bf3::host {

inline constexpr double kSquadJoinResendSeconds = 1.0;
inline constexpr float kSquadJoinMateMetres = 5.0f;

enum class SquadJoinPhase : uint8_t { Waiting, Switching, Joining, Deploying, Playing, Done, Failed };

const char *squad_join_phase_name(SquadJoinPhase p_phase);

struct SquadJoinView {
	std::span<const net::PlayerRow> players;
	uint8_t own_slot = 0;
	bool own_alive = false;
	uint32_t own_life = 0;
	sim::Vec3 own_position{};
	std::optional<sim::Vec3> mate_position;
	bool can_deploy = false;
	bool round_over = false;
};

struct SquadJoinIo {
	std::function<void(const net::SquadRequest &)> request;
	std::function<bool(uint8_t)> deploy_on;
};

std::string squad_list_text(std::span<const net::PlayerRow> p_players);
std::string players_text(std::span<const net::PlayerRow> p_players, uint8_t p_own_slot);

class SquadJoin {
public:
	SquadJoin(std::string p_mate, double p_timeout_s);

	SquadJoinPhase frame(double p_now_s, const SquadJoinView &p_view, const SquadJoinIo &p_io);
	SquadJoinPhase phase() const { return phase_; }
	int mate_slot() const { return mate_slot_; }
	uint32_t spawns() const { return spawns_; }
	uint32_t mate_spawns() const { return mate_spawns_; }
	const std::string &squads() const { return squads_; }
	const std::string &failure() const { return failure_; }
	std::string report() const;

private:
	void fail(const std::string &p_why);
	bool due(double p_now_s);

	std::string mate_;
	double timeout_s_ = 0.0;
	SquadJoinPhase phase_ = SquadJoinPhase::Waiting;
	double start_s_ = -1.0;
	double sent_s_ = -1.0;
	int mate_slot_ = -1;
	uint8_t own_slot_ = 0;
	uint8_t team_ = 0;
	uint8_t squad_ = 0;
	uint32_t life_ = 0;
	bool life_known_ = false;
	bool deploying_ = false;
	bool on_mate_ = false;
	uint32_t switches_ = 0;
	uint32_t joins_ = 0;
	uint32_t deploys_ = 0;
	uint32_t spawns_ = 0;
	uint32_t mate_spawns_ = 0;
	uint32_t waits_screen_ = 0;
	uint32_t waits_mate_ = 0;
	uint32_t waits_row_ = 0;
	float first_mate_m_ = -1.0f;
	std::string squads_;
	std::string failure_;
};

}

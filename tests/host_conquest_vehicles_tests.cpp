#include <doctest/doctest.h>

#include "bf3/ai/vehicle/vehicle_change.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"

#include "tdm_fixture.h"
#include "test_support.h"
#include "vehicles/server_vehicles.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

constexpr float kTakeSeconds = 30.0f;
constexpr uint32_t kM4Jeeps = 4;
constexpr uint32_t kM4Taken = 3;

struct JeepSeen {
	size_t spawner = 0;
	uint8_t team = 0;
	float initial_delay = 0.0f;
	sim::Tick live = 0;
	sim::Tick taken = 0;
	uint8_t taker = net::kNoVehicleOccupant;
	bool near_team = false;
};

struct JeepRun {
	std::string error;
	std::vector<JeepSeen> jeeps;
	sim::Tick in_round = 0;
	uint64_t choices = 0;
	double change_urgency = 0.0, winner_urgency = 0.0;
	uint64_t entries = 0;
};

sim::Tick take_limit() {
	return sim::seconds_to_ticks(kTakeSeconds);
}

JeepRun run_jeeps(const tt::TdmLevel &p_level, uint64_t p_seed) {
	JeepRun out;
	const host::LevelVehicles &vehicles = *p_level.shared->vehicles;
	for (size_t i = 0; i < vehicles.spawners.size(); ++i) {
		const host::LevelVehicleSpawner &s = vehicles.spawners[i];
		if (host::land_jeep(*vehicles.kinds[s.kind].model)) {
			out.jeeps.push_back(JeepSeen{ i, s.team, s.initial_spawn_delay });
		}
	}
	auto stats = std::make_shared<host::TdmBotStats>();
	tt::TdmRunConfig c;
	c.seed = p_seed;
	c.bots = 32;
	c.peer = false;
	c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = false;
	c.rules.revive = true;
	c.rules.preround_override_s = 0.0f;
	c.rules.max_players = 32;
	c.max_ticks = sim::seconds_to_ticks(tt::latest_initial_delay(vehicles)) + take_limit() + sim::kSimHz;
	c.after_round_ticks = 0;
	c.hashes = false;
	c.configure_bots = [&](host::TdmBotConfig &r_bots) {
		r_bots.tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		r_bots.change_unmerged = true;
		r_bots.stats = stats;
	};
	c.on_tick = [&](sim::Tick p_tick, host::GameServer &r_server, tt::ScriptedPeer *) {
		const host::ServerView &view = r_server.view();
		if (out.in_round == 0 && view.phase() == net::MatchPhase::InRound) {
			out.in_round = p_tick;
		}
		const host::ServerVehicles *live = view.server_vehicles();
		if (live == nullptr) {
			return;
		}
		out.entries = live->stats().entries;
		for (JeepSeen &j : out.jeeps) {
			const host::ServerVehicle &v = live->vehicles()[j.spawner];
			if (!v.live) {
				continue;
			}
			if (j.live == 0) {
				j.live = p_tick;
			}
			if (j.taken != 0) {
				continue;
			}
			const sim::Vec3 at = live->world()->state(sim::VehicleSlot(j.spawner)).body.com_position;
			const std::span<const sim::SoldierState> states = view.states();
			for (uint8_t slot = 0; slot < states.size() && !j.near_team; ++slot) {
				j.near_team = view.bot(slot) && view.alive(slot) && view.team(slot) == v.team &&
						tt::distance(states[slot].position, at) <= ai::kHardwareRadius;
			}
			for (const uint8_t occupant : v.occupants) {
				if (occupant != net::kNoVehicleOccupant && view.bot(occupant)) {
					j.taken = p_tick;
					j.taker = occupant;
					break;
				}
			}
		}
	};
	core::Result<tt::TdmRunResult> r = tt::run_tdm(p_level, c);
	if (!r) {
		out.error = r.error().message;
	}
	out.choices = stats->vehicle_choices.load();
	if (out.choices > 0) {
		out.change_urgency = double(stats->vehicle_change_milli.load()) / 1000.0 / double(out.choices);
		out.winner_urgency = double(stats->vehicle_winner_milli.load()) / 1000.0 / double(out.choices);
	}
	return out;
}

struct JeepCount {
	uint32_t near = 0;
	uint32_t taken = 0;
};

JeepCount count_jeeps(const JeepRun &p_run, uint64_t p_seed) {
	JeepCount n;
	for (const JeepSeen &j : p_run.jeeps) {
		const sim::Tick from = std::max(j.live, p_run.in_round);
		const bool in_time = j.taken != 0 && j.live != 0 && j.taken - from <= take_limit();
		n.near += j.near_team ? 1u : 0u;
		n.taken += j.near_team && in_time ? 1u : 0u;
		std::printf("host.tdm.conquest_vehicles: seed %llu: jeep spawner %zu team %u initial delay %.1f s, %s: live "
					"at tick %u, taken at tick %u by slot %u (%.2f s)\n",
				(unsigned long long)p_seed, j.spawner, unsigned(j.team), j.initial_delay,
				j.near_team ? "a team bot within 75 m" : "no team bot within 75 m", unsigned(j.live), unsigned(j.taken),
				unsigned(j.taker), j.taken != 0 ? double(j.taken - from) * double(sim::kTickSeconds) : -1.0);
	}
	std::printf("host.tdm.conquest_vehicles: seed %llu: %u of the %u jeeps with a team bot within 75 m (%zu jeeps in "
				"all) taken by bots within %.0f s, %llu entries, %llu change choices, mean change urgency before its "
				"weight %.3f, mean winning weighted urgency %.3f\n",
			(unsigned long long)p_seed, n.taken, n.near, p_run.jeeps.size(), double(kTakeSeconds),
			(unsigned long long)p_run.entries, (unsigned long long)p_run.choices, p_run.change_urgency,
			p_run.winner_urgency);
	return n;
}

void check_jeeps(uint64_t p_seed) {
	tt::TdmLevel *level = tt::load_conquest_bots_level_once();
	if (level == nullptr) {
		return;
	}
	REQUIRE(level->shared->vehicles != nullptr);
	const JeepRun run = run_jeeps(*level, p_seed);
	CHECK_MESSAGE(run.error.empty(), run.error);
	CHECK(run.in_round != 0u);
	const JeepCount n = count_jeeps(run, p_seed);
	CHECK(n.near >= kM4Jeeps);
	CHECK(n.taken * kM4Jeeps >= n.near * kM4Taken);
}

}

TEST_SUITE("host-tdm-conquest-vehicles") {
	TEST_CASE("conquest vehicles seed 1: bots take Caspian's jeeps through BBChange's choice") {
		check_jeeps(1);
	}
	TEST_CASE("conquest vehicles seed 2: bots take Caspian's jeeps through BBChange's choice") {
		check_jeeps(2);
	}
	TEST_CASE("conquest vehicles seed 3: bots take Caspian's jeeps through BBChange's choice") {
		check_jeeps(3);
	}
}

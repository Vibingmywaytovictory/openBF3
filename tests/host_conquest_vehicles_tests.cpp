#include <doctest/doctest.h>

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/ai/vehicle/vehicle_change.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/sim/soldier_motor.h"

#include "gunplay_fixture.h"
#include "tdm_fixture.h"
#include "test_support.h"
#include "vehicles/bot_crew.h"
#include "vehicles/server_vehicles.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;
namespace gp = bf3::test::gunplay;

namespace {

constexpr float kTakeSeconds = 30.0f;
constexpr uint32_t kM4Jeeps = 4;
constexpr uint32_t kM4Taken = 3;

struct JeepSeen {
	size_t spawner = 0;
	uint8_t team = 0;
	float initial_delay = 0.0f;
	bool near_team = false;
	sim::Tick live = 0;
	sim::Tick taken = 0;
	uint8_t taker = net::kNoVehicleOccupant;
	uint32_t bots_at_end = 0;
};

struct JeepRun {
	std::string error;
	std::vector<JeepSeen> jeeps;
	sim::Tick in_round = 0;
	uint64_t choices = 0;
	double change_urgency = 0.0, winner_urgency = 0.0;
	uint64_t foot_decisions = 0;
	double foot_stay = 0.0;
	uint64_t change_won = 0;
	host::ServerVehicleStats vehicles;
};

sim::Tick take_limit() {
	return sim::seconds_to_ticks(kTakeSeconds);
}

std::vector<std::string> words(const std::string &p_line) {
	std::istringstream in(p_line);
	std::vector<std::string> out;
	for (std::string w; in >> w;) {
		out.push_back(w);
	}
	return out;
}

struct ChangeScript {
	std::string text;
	bool declared_here = false;
};

std::vector<std::string> script_lines(const std::string &p_text) {
	std::istringstream in(p_text);
	std::vector<std::string> out;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		out.push_back(line);
	}
	return out;
}

ChangeScript declare_change(const std::string &p_behaviours) {
	const std::vector<std::string> lines = script_lines(p_behaviours);
	for (const std::string &line : lines) {
		const std::vector<std::string> w = words(line);
		if (w.size() == 3 && w[0] == "aiSettings.setBehaviour" && w[2] == "Change") {
			return { p_behaviours, false };
		}
	}
	std::ostringstream out;
	for (const std::string &line : lines) {
		const std::vector<std::string> w = words(line);
		if (w.size() == 2 && w[0] == "aiSettings.setMaxNumberOfBehaviours") {
			REQUIRE(std::stoul(w[1]) == ai::kInfantryBehaviourCount);
			out << w[0] << ' ' << ai::kBotBehaviourCount << '\n';
			out << "aiSettings.setBehaviour " << ai::kInfantryBehaviourCount << " Change\n";
			continue;
		}
		out << line << '\n';
		if (w.size() == 3 && w[0] == "aiSettings.setBehaviourModifier" && w[1] == "MoveTo") {
			out << w[0] << " Change " << w[2] << '\n';
		}
		if (w.size() == 8 && w[0] == "aiSettings.setVehicleBehaviour" && w[1] == "Infantry" && w[2] == "MoveTo") {
			out << w[0] << " Infantry Change BBChange BBPChange " << w[5] << ' ' << w[6] << ' ' << w[7] << '\n';
		}
	}
	return { out.str(), true };
}

ai::AiConfig change_declared_config(ChangeScript &r_script) {
	const std::filesystem::path root = OPENBF3_AI_DIR;
	ai::AiConfig config;
	REQUIRE(ai::run_ai_script(gp::read_text(root / ai::kAiDefaultScript), ai::kAiDefaultScript, config).ok());
	r_script = declare_change(gp::read_text(root / ai::kAiBehavioursScript));
	REQUIRE(ai::run_ai_script(r_script.text, ai::kAiBehavioursScript, config).ok());
	const std::filesystem::path level = ai::level_ai_script(root, "MP_007");
	REQUIRE(ai::run_ai_script(gp::read_text(level), level.string(), config).ok());
	return config;
}

void use_change_declared_ai(host::TdmBotConfig &r_bots) {
	REQUIRE(host::load_bot_tuning(OPENBF3_BOT_TUNING_CFG, r_bots).ok());
	ChangeScript script;
	const ai::AiConfig config = change_declared_config(script);
	ai::BehaviourTuning tuning = r_bots.behaviours;
	const core::Status st = ai::apply_behaviour_weights(config, tuning);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	REQUIRE(tuning.change);
	REQUIRE(config.behaviour_id("Change") == int(ai::InfantryBehaviour::Change));
	if (script.declared_here) {
		CHECK(tuning.curves[size_t(ai::InfantryBehaviour::Change)].weight ==
				tuning.curves[size_t(ai::InfantryBehaviour::MoveTo)].weight);
	}
	std::printf("host.tdm.conquest_vehicles: Change %s, weight %.3f\n",
			script.declared_here ? "declared by the row at MoveTo's modifier"
								 : "declared by the shipped AIBehaviours.ai",
			double(tuning.curves[size_t(ai::InfantryBehaviour::Change)].weight));
	r_bots.behaviours = tuning;
	r_bots.ai = config.settings;
}

JeepRun run_jeeps(const tt::TdmLevel &p_level, uint64_t p_seed) {
	JeepRun out;
	const host::LevelVehicles &vehicles = *p_level.shared->vehicles;
	for (size_t i = 0; i < vehicles.spawners.size(); ++i) {
		const host::LevelVehicleSpawner &s = vehicles.spawners[i];
		if (host::land_jeep(*vehicles.kinds[s.kind].model)) {
			JeepSeen j;
			j.spawner = i;
			j.team = s.team;
			j.initial_delay = s.initial_spawn_delay;
			out.jeeps.push_back(j);
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
		use_change_declared_ai(r_bots);
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
		out.vehicles = live->stats();
		for (JeepSeen &j : out.jeeps) {
			const host::ServerVehicle &v = live->vehicles()[j.spawner];
			j.bots_at_end = 0;
			if (!v.live) {
				continue;
			}
			if (j.live == 0) {
				j.live = p_tick;
			}
			const sim::Vec3 at = live->world()->state(sim::VehicleSlot(j.spawner)).body.com_position;
			const std::span<const sim::SoldierState> states = view.states();
			for (uint8_t slot = 0; j.taken == 0 && slot < states.size() && !j.near_team; ++slot) {
				j.near_team = view.bot(slot) && view.alive(slot) && view.team(slot) == v.team &&
						tt::distance(states[slot].position, at) <= ai::kHardwareRadius;
			}
			for (const uint8_t occupant : v.occupants) {
				if (occupant == net::kNoVehicleOccupant || !view.bot(occupant)) {
					continue;
				}
				++j.bots_at_end;
				if (j.taken == 0) {
					j.taken = p_tick;
					j.taker = occupant;
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
	out.foot_decisions = stats->vehicle_foot_decisions.load();
	out.change_won = stats->vehicle_change_won.load();
	if (out.foot_decisions > 0) {
		out.foot_stay = double(stats->vehicle_foot_stay_milli.load()) / 1000.0 / double(out.foot_decisions);
	}
	return out;
}

struct JeepCount {
	uint32_t near = 0;
	uint32_t near_taken = 0;
	uint32_t taken = 0;
};

JeepCount count_jeeps(const tt::TdmLevel &p_level, const JeepRun &p_run, uint64_t p_seed) {
	const host::LevelVehicles &vehicles = *p_level.shared->vehicles;
	JeepCount n;
	for (const host::LevelVehicleKind &kind : vehicles.kinds) {
		std::printf("host.tdm.conquest_vehicles: seed %llu: %s top speed %.1f m/s\n", (unsigned long long)p_seed,
				kind.blueprint.c_str(), host::change_vehicle_max_speed(*kind.model));
	}
	for (const JeepSeen &j : p_run.jeeps) {
		const sim::Tick from = std::max(j.live, p_run.in_round);
		const bool in_time = j.taken != 0 && j.live != 0 && j.taken - from <= take_limit();
		n.near += j.near_team ? 1u : 0u;
		n.near_taken += j.near_team && in_time ? 1u : 0u;
		n.taken += in_time ? 1u : 0u;
		const host::LevelVehicleKind &kind = vehicles.kinds[vehicles.spawners[j.spawner].kind];
		std::printf("host.tdm.conquest_vehicles: seed %llu: jeep spawner %zu %s (top speed %.1f m/s) team %u, %s, "
					"initial delay %.1f s: live at tick %u, taken at tick %u by slot %u (%.2f s), %u bots "
					"aboard at the end\n",
				(unsigned long long)p_seed, j.spawner, kind.blueprint.c_str(),
				host::change_vehicle_max_speed(*kind.model), unsigned(j.team),
				j.near_team ? "a team bot within 75 m" : "no team bot within 75 m", j.initial_delay, unsigned(j.live),
				unsigned(j.taken), unsigned(j.taker),
				j.taken != 0 ? double(j.taken - from) * double(sim::kTickSeconds) : -1.0, j.bots_at_end);
	}
	std::printf("host.tdm.conquest_vehicles: seed %llu: %u of the %u jeeps with a team bot within 75 m and %u of all "
				"%zu jeeps taken by bots "
				"within %.0f s; %llu entries, %llu exits, %llu seat switches; %llu change choices, mean change "
				"urgency before its weight %.3f, mean winning weighted urgency %.3f\n",
			(unsigned long long)p_seed, n.near_taken, n.near, n.taken, p_run.jeeps.size(), double(kTakeSeconds),
			(unsigned long long)p_run.vehicles.entries, (unsigned long long)p_run.vehicles.exits,
			(unsigned long long)p_run.vehicles.seat_switches, (unsigned long long)p_run.choices, p_run.change_urgency,
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
	const JeepCount n = count_jeeps(*level, run, p_seed);
	CHECK(n.near >= kM4Jeeps);
	CHECK(n.near_taken * kM4Jeeps >= n.near * kM4Taken);
	const float sprint = sim::sprint_speed(level->shared->motor);
	std::printf("host.tdm.conquest_vehicles: seed %llu: soldier sprint speed %.2f m/s; %llu decisions on foot, mean "
				"staying-on-foot urgency %.3f; Change won %llu decisions\n",
			(unsigned long long)p_seed, double(sprint), (unsigned long long)run.foot_decisions, run.foot_stay,
			(unsigned long long)run.change_won);
	CHECK(sprint > 0.0f);
	CHECK(run.foot_decisions > 0u);
	CHECK(run.foot_stay > 0.0);
	CHECK(run.foot_stay <= 10.0 * double(sprint));
	CHECK(run.change_won > 0u);
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

TEST_SUITE("host-vehicles-possession") {
	TEST_CASE("vehicles: a seat's possession clock is the tick a player who is not a bot left it, and a bot's entry or "
			  "exit leaves it") {
		tt::TdmLevel *level = tt::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::optional<size_t> jeep;
		for (size_t i = 0; i < lv.spawners.size() && !jeep; ++i) {
			const host::LevelVehicleKind &k = lv.kinds[lv.spawners[i].kind];
			if (host::land_jeep(*k.model) && k.seats >= 2) {
				jeep = i;
			}
		}
		REQUIRE(jeep.has_value());
		auto vehicles = std::make_shared<host::LevelVehicles>(lv);
		vehicles->spawners[*jeep].initial_spawn_delay = 0.0f;
		host::ServerVehicles sv;
		REQUIRE(sv.configure(vehicles, level->shared->static_world, 1.0f).ok());
		sim::Tick t = 1;
		sv.spawn_due(t, nullptr);
		REQUIRE(sv.vehicles()[*jeep].live);
		sim::SoldierState soldier;
		soldier.position = sv.world()->state(sim::VehicleSlot(*jeep)).body.com_position;
		constexpr uint8_t kBot = 3;
		constexpr uint8_t kPlayer = 4;
		constexpr uint8_t kRemovedBot = 5;
		bool removed = false;
		const auto is_bot = [&](uint8_t p_slot) { return p_slot == kBot || (p_slot == kRemovedBot && !removed); };
		const auto press_use = [&](uint8_t p_slot) {
			(void)sv.use(p_slot, sim::Command{}, soldier);
			sim::Command use;
			use.buttons = sim::buttons::kUse;
			(void)sv.use(p_slot, use, soldier);
			++t;
			REQUIRE(sv.step(t, 0, is_bot).ok());
		};
		const auto stamps = [&]() {
			uint32_t n = 0;
			for (const std::optional<sim::Tick> &left : sv.vehicles()[*jeep].player_left) {
				n += left.has_value() ? 1u : 0u;
			}
			return n;
		};
		press_use(kBot);
		REQUIRE(sv.seat_of(kBot).has_value());
		CHECK(sv.seat_of(kBot)->vehicle == *jeep);
		const uint8_t seat = sv.seat_of(kBot)->seat;
		press_use(kBot);
		CHECK_FALSE(sv.seat_of(kBot).has_value());
		CHECK(stamps() == 0u);
		press_use(kPlayer);
		REQUIRE(sv.seat_of(kPlayer).has_value());
		CHECK(sv.seat_of(kPlayer)->seat == seat);
		CHECK(stamps() == 0u);
		press_use(kPlayer);
		const sim::Tick left = t;
		CHECK(stamps() == 1u);
		CHECK(sv.vehicles()[*jeep].player_left[seat] == std::optional<sim::Tick>(left));
		press_use(kBot);
		REQUIRE(sv.seat_of(kBot).has_value());
		press_use(kBot);
		CHECK(stamps() == 1u);
		CHECK(sv.vehicles()[*jeep].player_left[seat] == std::optional<sim::Tick>(left));
		press_use(kRemovedBot);
		REQUIRE(sv.seat_of(kRemovedBot).has_value());
		removed = true;
		sv.leave(kRemovedBot);
		++t;
		REQUIRE(sv.step(t, 0, is_bot).ok());
		CHECK_FALSE(sv.seat_of(kRemovedBot).has_value());
		CHECK(stamps() == 1u);
		CHECK(sv.vehicles()[*jeep].player_left[seat] == std::optional<sim::Tick>(left));
	}
}

TEST_SUITE("host-tdm-conquest-vehicles-unit") {
	TEST_CASE("the jeep rows' config declares Change once: on a script that declares it the helper changes nothing") {
		ChangeScript first;
		const ai::AiConfig config = change_declared_config(first);
		ai::BehaviourTuning tuning;
		REQUIRE(ai::apply_behaviour_weights(config, tuning).ok());
		CHECK(tuning.change);
		CHECK(config.behaviour_id("Change") == int(ai::InfantryBehaviour::Change));
		const ChangeScript again = declare_change(first.text);
		CHECK_FALSE(again.declared_here);
		CHECK(again.text == first.text);
	}
}

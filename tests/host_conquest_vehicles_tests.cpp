#include <doctest/doctest.h>

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/ai/vehicle/vehicle_change.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/sim/collision_world.h"
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
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;
namespace gp = bf3::test::gunplay;

namespace {

constexpr float kTakeSeconds = 30.0f;
constexpr float kExitSeconds = 600.0f;
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
	sim::Vec3 taken_at{};
	float moved_m = 0.0f;
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
	uint64_t drive_ticks = 0;
	uint64_t stuck_marks = 0;
	sim::Tick first_exit = 0;
	uint64_t get_outs = 0;
	uint64_t class_switches = 0;
	uint64_t fire_ticks = 0;
	uint64_t seat_shots = 0;
	uint64_t seat_hits = 0;
	sim::Tick first_fire = 0;
	host::ServerVehicleStats vehicles;
};

sim::Tick take_limit() {
	return sim::seconds_to_ticks(kTakeSeconds);
}

JeepRun run_jeeps(const tt::TdmLevel &p_level, uint64_t p_seed, bool p_seat_fire) {
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
	const sim::Tick take_end = sim::seconds_to_ticks(tt::latest_initial_delay(vehicles)) + take_limit() + sim::kSimHz;
	c.max_ticks = take_end + sim::seconds_to_ticks(kExitSeconds);
	c.until = [&](sim::Tick p_tick) {
		return p_tick >= take_end && stats->vehicle_get_outs.load() > 0 && (!p_seat_fire || out.seat_hits > 0);
	};
	c.after_round_ticks = 0;
	c.hashes = false;
	c.configure_bots = [&](host::TdmBotConfig &r_bots) {
		CHECK(r_bots.behaviours.change);
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
		out.seat_shots = live->gunnery().stats().shots;
		out.seat_hits = live->gunnery().stats().damage;
		if (out.first_exit == 0 && stats->vehicle_get_outs.load() > 0) {
			out.first_exit = p_tick;
		}
		if (out.first_fire == 0 && stats->vehicle_fire_ticks.load() > 0) {
			out.first_fire = p_tick;
		}
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
			const bool in_window = p_tick <= std::max(j.live, out.in_round) + take_limit();
			if (j.taken != 0) {
				j.moved_m = std::max(j.moved_m, tt::distance(j.taken_at, at));
			}
			const std::span<const sim::SoldierState> states = view.states();
			for (uint8_t slot = 0; in_window && j.taken == 0 && slot < states.size() && !j.near_team; ++slot) {
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
					j.taken_at = at;
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
	out.drive_ticks = stats->vehicle_drive_ticks.load();
	out.stuck_marks = stats->vehicle_stuck_marks.load();
	out.get_outs = stats->vehicle_get_outs.load();
	out.class_switches = stats->vehicle_class_switches.load();
	out.fire_ticks = stats->vehicle_fire_ticks.load();
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
					"aboard at the end, moved up to %.1f m from where it was taken\n",
				(unsigned long long)p_seed, j.spawner, kind.blueprint.c_str(),
				host::change_vehicle_max_speed(*kind.model), unsigned(j.team),
				j.near_team ? "a team bot within 75 m" : "no team bot within 75 m", j.initial_delay, unsigned(j.live),
				unsigned(j.taken), unsigned(j.taker),
				j.taken != 0 ? double(j.taken - from) * double(sim::kTickSeconds) : -1.0, j.bots_at_end,
				double(j.moved_m));
	}
	std::printf("host.tdm.conquest_vehicles: seed %llu: %u of the %u jeeps with a team bot within 75 m and %u of all "
				"%zu jeeps taken by bots "
				"within %.0f s; %llu entries, %llu exits, %llu seat switches; %llu change choices, mean change "
				"urgency before its weight %.3f, mean winning weighted urgency %.3f; %llu driving ticks, %llu stuck "
				"marks; %llu water throw-outs; %llu bot get-outs, the first at tick %u\n",
			(unsigned long long)p_seed, n.near_taken, n.near, n.taken, p_run.jeeps.size(), double(kTakeSeconds),
			(unsigned long long)p_run.vehicles.entries, (unsigned long long)p_run.vehicles.exits,
			(unsigned long long)p_run.vehicles.seat_switches, (unsigned long long)p_run.choices, p_run.change_urgency,
			p_run.winner_urgency, (unsigned long long)p_run.drive_ticks, (unsigned long long)p_run.stuck_marks,
			(unsigned long long)p_run.vehicles.water_throw_outs, (unsigned long long)p_run.get_outs,
			unsigned(p_run.first_exit));
	return n;
}

void check_jeeps(tt::TdmLevel *p_level, uint64_t p_seed, bool p_seat_fire) {
	tt::TdmLevel *level = p_level;
	if (level == nullptr) {
		return;
	}
	REQUIRE(level->shared->vehicles != nullptr);
	const JeepRun run = run_jeeps(*level, p_seed, p_seat_fire);
	CHECK_MESSAGE(run.error.empty(), run.error);
	CHECK(run.in_round != 0u);
	const JeepCount n = count_jeeps(*level, run, p_seed);
	CHECK(n.near >= kM4Jeeps);
	CHECK(n.near_taken * kM4Jeeps >= n.near * kM4Taken);
	CHECK(run.vehicles.entries > 0u);
	CHECK(run.get_outs > 0u);
	const float sprint = sim::sprint_speed(level->shared->motor);
	std::printf("host.tdm.conquest_vehicles: seed %llu: soldier sprint speed %.2f m/s; %llu decisions on foot, mean "
				"staying-on-foot urgency %.3f; Change won %llu decisions; %llu equipment class switches; %llu ticks a "
				"seated bot held a seat's trigger (the first at tick %u), %llu vehicle weapon shots, %llu soldier "
				"hits by them\n",
			(unsigned long long)p_seed, double(sprint), (unsigned long long)run.foot_decisions, run.foot_stay,
			(unsigned long long)run.change_won, (unsigned long long)run.class_switches,
			(unsigned long long)run.fire_ticks, unsigned(run.first_fire), (unsigned long long)run.seat_shots,
			(unsigned long long)run.seat_hits);
	CHECK(sprint > 0.0f);
	CHECK(run.foot_decisions > 0u);
	CHECK(run.foot_stay > 0.0);
	CHECK(run.foot_stay > 10.0 * double(sprint));
	CHECK(run.change_won > 0u);
	CHECK(run.class_switches > 0u);
	if (p_seat_fire) {
		CHECK(run.fire_ticks > 0u);
		CHECK(run.seat_shots > 0u);
		CHECK(run.seat_hits > 0u);
	}
}
}

TEST_SUITE("host-tdm-conquest-vehicles") {
	TEST_CASE("conquest vehicles seed 1: bots take Caspian's jeeps through BBChange's choice, and a seated bot "
			  "fires its vehicle weapon and hits a soldier") {
		check_jeeps(tt::load_conquest_bots_level_once(), 1, true);
	}
	TEST_CASE("conquest vehicles seed 2: bots take Caspian's jeeps through BBChange's choice, and a seated bot "
			  "fires its vehicle weapon and hits a soldier") {
		check_jeeps(tt::load_conquest_bots_level_once(), 2, true);
	}
	TEST_CASE("conquest vehicles seed 3: bots take Caspian's jeeps through BBChange's choice, and a seated bot "
			  "fires its vehicle weapon and hits a soldier") {
		check_jeeps(tt::load_conquest_bots_level_once(), 3, true);
	}
	TEST_CASE("conquest large vehicles seed 1: bots take Caspian Conquest Large's jeeps through BBChange's choice") {
		check_jeeps(tt::load_conquest_large_bots_level_once(), 1, false);
	}
	TEST_CASE("conquest large vehicles seed 2: bots take Caspian Conquest Large's jeeps through BBChange's choice") {
		check_jeeps(tt::load_conquest_large_bots_level_once(), 2, false);
	}
	TEST_CASE("conquest large vehicles seed 3: bots take Caspian Conquest Large's jeeps through BBChange's choice") {
		check_jeeps(tt::load_conquest_large_bots_level_once(), 3, false);
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
		core::Result<std::unique_ptr<sim::CollisionWorld>> world =
				sim::CollisionWorld::create(level->shared->static_world);
		REQUIRE(world.ok());
		sim::SoldierMotor motor;
		REQUIRE(motor.init(**world, level->shared->motor).ok());
		sim::SoldierState soldier;
		soldier.position = sv.world()->state(sim::VehicleSlot(*jeep)).body.com_position;
		constexpr uint8_t kBot = 3;
		constexpr uint8_t kPlayer = 4;
		constexpr uint8_t kRemovedBot = 5;
		bool removed = false;
		const auto is_bot = [&](uint8_t p_slot) { return p_slot == kBot || (p_slot == kRemovedBot && !removed); };
		const auto press_use = [&](uint8_t p_slot) {
			(void)sv.use(p_slot, sim::Command{}, soldier, motor);
			sim::Command use;
			use.buttons = sim::buttons::kUse;
			(void)sv.use(p_slot, use, soldier, motor);
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
	TEST_CASE("the jeep rows run the shipped bots, whose AIBehaviours.ai declares Change") {
		host::TdmBotConfig bots;
		test::use_shipped_bot_ai(bots);
		CHECK(bots.behaviours.change);
		CHECK(bots.behaviours.curves[size_t(ai::InfantryBehaviour::Change)].weight > 0.0f);
	}
}

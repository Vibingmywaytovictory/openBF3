#include <doctest/doctest.h>

#include "bf3/host/game_server.h"
#include "bf3/host/squads.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/sim/logic/conquest_natives.h"

#include "tdm_fixture.h"
#include "tdm_strategy.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

constexpr uint32_t kRoundTicks = 60u * 60u * 25u;
constexpr sim::Tick kHeldBeforeOrderTicks = 120;
constexpr float kSpawnMatchMetres = 3.0f;

tt::TdmLevel *load_conquest_bots_level_once() {
	static std::unique_ptr<tt::TdmLevel> s_level;
	static bool s_tried = false;
	if (s_tried) {
		if (s_level == nullptr) {
			bf3::test::skip_install_case("no BF3 install or no Conquest bot level (see the first case)");
		}
		return s_level.get();
	}
	s_tried = true;
	s_level = tt::load_tdm_level("MP_007", "ConquestSmall0", true);
	return s_level.get();
}

float flat_distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return std::hypot(p_a[0] - p_b[0], p_a[2] - p_b[2]);
}

int point_of_spawn(const host::SharedLevel &p_shared, size_t p_spawn) {
	const sim::logic::ConquestParams &params = *p_shared.logic_natives->conquest;
	const uint32_t object = p_shared.spawn_spawners[p_spawn];
	for (const sim::logic::ConquestSpawnerData &spawner : params.spawners) {
		if (spawner.level_object != object) {
			continue;
		}
		for (size_t p = 0; p < params.points.size(); ++p) {
			const std::vector<uint32_t> &own = params.points[p].spawners;
			if (std::find(own.begin(), own.end(), spawner.node) != own.end()) {
				return int(p);
			}
		}
	}
	return -1;
}

uint8_t holder(const sim::logic::ConquestPointView &p_point) {
	return p_point.controlled ? p_point.team : uint8_t(0);
}

struct OrderSeen {
	sim::Tick tick = 0;
	uint8_t team = 0;
	net::SquadOrderKind kind = net::SquadOrderKind::None;
	int point = -1;
	uint8_t held_by = 0;
	bool held_long = false;
	float progress = 0.0f;
	sim::Tick held_for = 0;
};

struct ConquestRun {
	tt::TdmRunResult result;
	std::string error;
	std::array<int64_t, 3> tickets{};
	std::array<uint32_t, 3> captures{};
	uint32_t hands_changed = 0;
	std::vector<OrderSeen> orders;
	uint32_t bot_spawns = 0, flag_spawns = 0, hq_spawns = 0, other_spawns = 0;
	uint64_t squad_spawns = 0;
	uint64_t runtime_faults = 0;
	std::vector<int> area_point;
	std::vector<std::string> log;
};

ConquestRun run_conquest(const tt::TdmLevel &p_level, uint64_t p_seed) {
	ConquestRun out;
	const host::SharedLevel &shared = *p_level.shared;
	const host::BotLevel &bots = *p_level.bots;
	tt::TdmRunConfig c;
	c.seed = p_seed;
	c.bots = 32;
	c.peer = false;
	c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = false;
	c.rules.revive = true;
	c.rules.preround_override_s = 0.0f;
	c.rules.max_players = 32;
	c.max_ticks = kRoundTicks;
	c.after_round_ticks = 0;
	c.hashes = false;
	c.configure_bots = [](host::TdmBotConfig &r_bots) { r_bots.tuning_cfg = OPENBF3_BOT_TUNING_CFG; };
	std::vector<uint8_t> last_holder;
	std::vector<uint8_t> last_team;
	std::vector<sim::Tick> held_since;
	std::array<std::array<host::SquadOrderState, net::kMaxSquads + 1>, host::kSquadTeams + 1> orders{};
	std::array<uint32_t, 64> seen_life{};
	c.on_tick = [&](sim::Tick p_tick, host::GameServer &r_server, tt::ScriptedPeer *) {
		const host::TdmRules *rules = r_server.rules();
		const sim::logic::ConquestNatives *cq = rules != nullptr ? rules->natives().conquest() : nullptr;
		if (cq == nullptr) {
			return;
		}
		const std::span<const sim::logic::ConquestPointView> points = cq->points();
		if (out.area_point.empty() && !points.empty()) {
			out.area_point = host::match_control_points(bots.areas, points);
			last_holder.assign(points.size(), 0);
			last_team.assign(points.size(), 0);
			held_since.assign(points.size(), 0);
			for (size_t i = 0; i < points.size(); ++i) {
				last_holder[i] = holder(points[i]);
				last_team[i] = last_holder[i];
			}
		}
		for (size_t i = 0; i < points.size() && i < last_holder.size(); ++i) {
			const uint8_t now = holder(points[i]);
			if (now != last_holder[i]) {
				if (now == 1 || now == 2) {
					++out.captures[now];
					out.hands_changed += last_team[i] != 0 && last_team[i] != now ? 1u : 0u;
					last_team[i] = now;
					out.log.push_back("tick " + std::to_string(p_tick) + ": point " + std::to_string(i) + " to team " +
							std::to_string(now));
				}
				if (now != 0) {
					held_since[i] = p_tick;
				}
				last_holder[i] = now;
			}
		}
		for (uint8_t team = 1; team <= host::kSquadTeams; ++team) {
			for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
				const host::SquadOrderState &o = rules->squads().order(team, q);
				host::SquadOrderState &before = orders[team][q];
				if (o.kind == before.kind && o.area == before.area) {
					continue;
				}
				before = o;
				if (o.kind != net::SquadOrderKind::Attack && o.kind != net::SquadOrderKind::Defend) {
					continue;
				}
				OrderSeen seen;
				seen.tick = p_tick;
				seen.team = team;
				seen.kind = o.kind;
				seen.point = o.area < out.area_point.size() ? out.area_point[o.area] : -1;
				if (seen.point >= 0) {
					seen.held_by = holder(points[size_t(seen.point)]);
					seen.progress = points[size_t(seen.point)].progress;
					seen.held_for = p_tick - held_since[size_t(seen.point)];
					seen.held_long =
							seen.held_by != 0 && p_tick >= held_since[size_t(seen.point)] + kHeldBeforeOrderTicks;
				}
				out.orders.push_back(seen);
			}
		}
		const std::span<const sim::SoldierState> states = r_server.view().states();
		for (const net::PlayerRow &row : rules->players()) {
			if (!row.bot || row.slot >= seen_life.size() || row.slot >= states.size() ||
					!r_server.view().alive(row.slot) || seen_life[row.slot] == states[row.slot].life + 1u) {
				continue;
			}
			seen_life[row.slot] = states[row.slot].life + 1u;
			++out.bot_spawns;
			int spawn = -1;
			float best = kSpawnMatchMetres;
			for (size_t s = 0; s < shared.spawns.size(); ++s) {
				const float d = flat_distance(shared.spawns[s].position, states[row.slot].position);
				if (d < best) {
					best = d;
					spawn = int(s);
				}
			}
			const int point = spawn >= 0 ? point_of_spawn(shared, size_t(spawn)) : -1;
			if (point < 0 || size_t(point) >= points.size()) {
				++out.other_spawns;
			} else if (points[size_t(point)].hq) {
				++out.hq_spawns;
			} else {
				++out.flag_spawns;
			}
		}
		const sim::logic::ConquestNatives &n = *cq;
		out.tickets[1] = n.tickets(1);
		out.tickets[2] = n.tickets(2);
		out.squad_spawns = rules->stats().squad_spawns;
		out.runtime_faults = rules->stats().runtime_faults;
	};
	core::Result<tt::TdmRunResult> r = tt::run_tdm(p_level, c);
	if (!r) {
		out.error = r.error().message;
		return out;
	}
	out.result = std::move(*r);
	return out;
}

}

TEST_SUITE("host-tdm-conquest-bots-unit") {
	TEST_CASE("conquest bots: a capture point's owner and take-over counter for the bots' strategic areas") {
		sim::logic::ConquestPointView held;
		held.team = 2;
		held.controlled = true;
		held.progress = sim::logic::kFlagTop;
		const host::ControlPointState full = host::control_point_state(held);
		CHECK(full.owner == 2);
		CHECK(full.take_over == doctest::Approx(1.0f));
		held.team = 1;
		held.progress = 45.0f;
		const host::ControlPointState pulled = host::control_point_state(held);
		CHECK(pulled.owner == 1);
		CHECK(pulled.take_over == doctest::Approx(0.45f));
		sim::logic::ConquestPointView rising;
		rising.team = 2;
		rising.controlled = false;
		rising.progress = 60.0f;
		const host::ControlPointState neutral = host::control_point_state(rising);
		CHECK(neutral.owner == 0);
		CHECK(neutral.take_over == doctest::Approx(1.0f));
	}
	TEST_CASE("conquest bots: flag and HQ areas find the capture point placed by their objective's level object, "
			  "wherever it stands; TDM areas and unplaced objectives none") {
		std::vector<host::BotStrategicArea> areas(5);
		areas[0].kind = host::BotStrategicArea::Kind::Flag;
		areas[0].level_object = 40;
		areas[0].centre = { 0.0f, 5.0f, 0.0f };
		areas[1].kind = host::BotStrategicArea::Kind::Hq;
		areas[1].level_object = 7;
		areas[1].centre = { 100.0f, 0.0f, 0.0f };
		areas[2].kind = host::BotStrategicArea::Kind::TdmCluster;
		areas[2].level_object = 12;
		areas[2].centre = { 20.0f, 0.0f, 20.0f };
		areas[3].kind = host::BotStrategicArea::Kind::Flag;
		areas[3].level_object = 12;
		areas[3].centre = { 500.0f, 0.0f, 500.0f };
		areas[4].kind = host::BotStrategicArea::Kind::Flag;
		areas[4].level_object = 99;
		areas[4].centre = { 101.0f, 0.0f, 0.0f };
		std::vector<sim::logic::ConquestPointView> points(4);
		points[0].hq = true;
		points[0].level_object = 7;
		points[0].position = { 101.0f, 0.0f, 0.0f };
		points[1].level_object = 40;
		points[1].position = { 2.0f, 30.0f, 1.0f };
		points[2].level_object = 12;
		points[2].position = { 20.0f, 0.0f, 20.0f };
		points[3].position = { 0.0f, 5.0f, 0.0f };
		const std::vector<int> matched = host::match_control_points(areas, points);
		REQUIRE(matched.size() == areas.size());
		CHECK(matched[0] == 1);
		CHECK(matched[1] == 0);
		CHECK(matched[2] == -1);
		CHECK(matched[3] == 2);
		CHECK(matched[4] == -1);
		CHECK(host::match_control_points(areas, {}) == std::vector<int>(areas.size(), -1));
	}
}

TEST_SUITE("host-tdm-conquest-bots") {
	TEST_CASE("conquest bots: MP_007 ConquestSmall0 with 32 bots: squads attack the flags their team does not hold and "
			  "defend the ones it holds, flags change hands, bots spawn on flags and on squad mates, a team wins on "
			  "tickets") {
		tt::TdmLevel *level = load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		REQUIRE(level->shared->logic_natives != nullptr);
		REQUIRE(level->shared->logic_natives->conquest != nullptr);
		const ConquestRun run = run_conquest(*level, 1);
		REQUIRE_MESSAGE(run.error.empty(), run.error);
		std::vector<int> paired;
		for (size_t i = 0; i < level->bots->areas.size(); ++i) {
			const host::BotStrategicArea::Kind kind = level->bots->areas[i].kind;
			if (kind == host::BotStrategicArea::Kind::Flag || kind == host::BotStrategicArea::Kind::Hq) {
				CHECK_MESSAGE(i < run.area_point.size(), "area ", i, " has no capture point");
				if (i < run.area_point.size()) {
					CHECK_MESSAGE(run.area_point[i] >= 0, "area ", i, " has no capture point");
					paired.push_back(run.area_point[i]);
				}
			}
		}
		std::sort(paired.begin(), paired.end());
		CHECK(std::adjacent_find(paired.begin(), paired.end()) == paired.end());
		CHECK(paired.size() == level->shared->logic_natives->conquest->points.size());
		for (const std::string &line : run.log) {
			std::printf("host.tdm.conquest_bots: %s\n", line.c_str());
		}
		uint32_t attack = 0, defend = 0, attack_held = 0, defend_held = 0, defend_other = 0;
		for (const OrderSeen &o : run.orders) {
			if (o.kind == net::SquadOrderKind::Attack) {
				++attack;
				attack_held += o.held_long && o.held_by == o.team ? 1u : 0u;
				if (o.held_long && o.held_by == o.team) {
					std::printf(
							"host.tdm.conquest_bots: tick %u team %u attack on point %d held %u ticks, progress %.1f\n",
							unsigned(o.tick), unsigned(o.team), o.point, unsigned(o.held_for), o.progress);
				}
			} else {
				++defend;
				defend_held += o.held_by == o.team ? 1u : 0u;
				defend_other += o.held_long && o.held_by != o.team ? 1u : 0u;
			}
		}
		std::string mapping;
		for (const int p : run.area_point) {
			mapping += " " + std::to_string(p);
		}
		std::printf(
				"host.tdm.conquest_bots: areas to points%s; round over %d at tick %u, winner %u; tickets US %lld RU "
				"%lld; captures US %u RU %u, %u from the other team; orders attack %u (on a flag the team held %u), "
				"defend %u (on a held "
				"flag %u, on another %u); bot spawns %u: HQ %u, flag %u, other %u; squad spawns %llu; kills %d / %d; "
				"runtime faults %llu; %.1f s\n",
				mapping.c_str(), int(run.result.round_over), unsigned(run.result.round_over_tick),
				unsigned(run.result.winner), (long long)run.tickets[1], (long long)run.tickets[2], run.captures[1],
				run.captures[2], run.hands_changed, attack, attack_held, defend, defend_held, defend_other,
				run.bot_spawns, run.hq_spawns, run.flag_spawns, run.other_spawns, (unsigned long long)run.squad_spawns,
				run.result.kills[1], run.result.kills[2], (unsigned long long)run.runtime_faults,
				run.result.seconds_run);
		CHECK(run.result.failure.empty());
		CHECK(run.result.round_over);
		CHECK((run.result.winner == 1 || run.result.winner == 2));
		if (run.result.winner == 1 || run.result.winner == 2) {
			CHECK(run.tickets[run.result.winner] > 0);
			CHECK(run.tickets[3 - run.result.winner] == 0);
		}
		CHECK(run.captures[1] > 0u);
		CHECK(run.captures[2] > 0u);
		CHECK(run.hands_changed > 0u);
		CHECK(attack > 0u);
		CHECK(attack_held == 0u);
		CHECK(defend_held > 0u);
		CHECK(defend_other == 0u);
		CHECK(run.flag_spawns > 0u);
		CHECK(run.squad_spawns > 0u);
		CHECK(run.runtime_faults == 0u);
	}
	TEST_CASE("host-tdm-conquest-bots: install unchanged") {
		tt::TdmLevel *level = load_conquest_bots_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}

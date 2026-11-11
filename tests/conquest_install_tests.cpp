#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/logic/conquest_data.h"
#include "bf3/sim/logic/conquest_natives.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/logic/round_natives.h"
#include "test_support.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core::gameplay;
using namespace bf3::sim::logic;

namespace {

struct ConquestLevel {
	std::unique_ptr<core::level::LoadedLevel> level;
	std::unique_ptr<core::level::World> world;
	std::shared_ptr<const LogicGraph> graph;
	std::shared_ptr<const ConquestParams> params;
};

std::optional<ConquestLevel> load_conquest(
		test::Mounted &p_mount, const std::string &p_level, const std::string &p_mode) {
	ConquestLevel out;
	core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded =
			core::level::load_level(p_mount.install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	out.level = std::move(*loaded);
	core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(*out.level);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	out.world = std::move(*world);
	LogicWorldOptions wo;
	wo.mode = out.level->mode;
	wo.expand_root_objects = true;
	core::Result<LogicWorld> lw = LogicWorld::build(out.level->db(), out.level->root(), wo);
	REQUIRE_MESSAGE(lw.ok(), (lw.ok() ? std::string() : lw.error().message));
	LogicGraphOptions go;
	go.mode = out.level->mode;
	go.side = LogicSide::Server;
	core::Result<LogicGraph> g = build_logic_graph(*lw, go);
	REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
	out.graph = std::make_shared<const LogicGraph>(std::move(*g));
	core::Result<std::shared_ptr<const ConquestParams>> params = read_conquest_params(*lw, *out.graph, out.world.get());
	REQUIRE_MESSAGE(params.ok(), (params.ok() ? std::string() : params.error().message));
	out.params = *params;
	for (const std::string &n : out.params->notes) {
		std::printf("%s %s: %s\n", p_level.c_str(), p_mode.c_str(), n.c_str());
	}
	return out;
}

struct Match {
	std::optional<LogicRuntime> runtime;
	std::optional<RoundNatives> natives;
	sim::Tick tick = 0;

	void step(std::span<const ConquestPlayerSample> p_players) {
		const sim::Tick next = runtime->tick() + 1u;
		REQUIRE(natives->apply(next, runtime->effects(), *runtime).ok());
		REQUIRE(natives->tick_round(next, *runtime).ok());
		REQUIRE(natives->conquest_step(next, p_players, *runtime).ok());
		REQUIRE(runtime->step().ok());
		REQUIRE_FALSE(runtime->faulted());
		tick = next;
	}
};

Match start_match(const ConquestLevel &p_level) {
	Match m;
	core::Result<LogicRuntime> rt = LogicRuntime::create(p_level.graph, LogicRuntimeOptions{});
	REQUIRE(rt.ok());
	auto params = std::make_shared<RoundNativeParams>();
	params->conquest = p_level.params;
	RoundNativeConfig config;
	config.params = params;
	config.min_players_override = 2;
	config.preround_override_s = 0.0f;
	core::Result<RoundNatives> natives = RoundNatives::create(p_level.graph, config);
	REQUIRE_MESSAGE(natives.ok(), (natives.ok() ? std::string() : natives.error().message));
	m.runtime.emplace(std::move(*rt));
	m.natives.emplace(std::move(*natives));
	REQUIRE(m.runtime->start().ok());
	return m;
}

const ConquestPointData *hq_of(const ConquestParams &p_params, uint8_t p_team) {
	for (const ConquestPointData &p : p_params.points) {
		if (p.params.type == CapturableType::UnableToChangeTeam && p.params.initial_owner == p_team) {
			return &p;
		}
	}
	return nullptr;
}

}

TEST_SUITE("logic-conquest") {
	TEST_CASE("logic.conquest: MP_007 ConquestSmall0 reads 4 flags, 2 HQs, 2 ticket counters and 2 team combat areas "
			  "from the mode") {
		test::Mounted *mount = test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		const std::optional<ConquestLevel> l = load_conquest(*mount, "MP_007", "ConquestSmall0");
		const ConquestParams &p = *l->params;
		size_t flags = 0, hqs = 0;
		for (const ConquestPointData &d : p.points) {
			CHECK(d.placed);
			if (d.params.type == CapturableType::UnableToChangeTeam) {
				++hqs;
				CHECK_FALSE(d.params.start_at_bottom);
				CHECK(d.params.area_value == 0.0f);
				continue;
			}
			++flags;
			CHECK(d.params.time_to_get_control == 20);
			CHECK(d.params.time_to_lose_control == 20);
			CHECK(d.params.max_capture_multiplier == 4);
			CHECK(d.params.min_nr_to_take_control == 1);
			CHECK(d.params.added_multiplier_per_player == doctest::Approx(0.34f));
			CHECK(d.params.return_multiplier == doctest::Approx(1.0f));
			CHECK(d.params.seesaw);
			CHECK(d.params.start_at_bottom);
			CHECK(d.params.area_value == doctest::Approx(34.0f));
			CHECK(d.params.initial_owner == 0);
			CHECK_FALSE(d.params.volumes.empty());
			CHECK(capture_point_contains(d.params, d.params.position));
			size_t character = 0;
			for (const uint32_t s : d.spawners) {
				const ConquestSpawnerData *sp = p.spawner(s);
				REQUIRE(sp != nullptr);
				if (!sp->vehicle) {
					++character;
					CHECK(sp->gate.locked_team);
					CHECK_FALSE(sp->gate.enabled);
				}
			}
			CHECK(character == 2);
		}
		CHECK(flags == 4);
		CHECK(hqs == 2);
		REQUIRE(p.counters.size() == 2);
		for (const ConquestCounterData &c : p.counters) {
			CHECK(c.params.initial_ticket_count == 150);
			CHECK(c.params.loss_per_min == doctest::Approx(15.0f));
			CHECK(c.params.loss_start == doctest::Approx(100.0f));
			CHECK(c.params.loss_when_lost_all == doctest::Approx(999.0f));
			CHECK(c.params.halt_on_equal_point_count);
			CHECK(c.params.decrease == TicketDecrease::OnRespawned);
			CHECK_FALSE(c.params.spawn_always_allowed);
			CHECK(c.params.percentages.empty());
		}
		CHECK(p.counters[0].params.team != p.counters[1].params.team);
		REQUIRE(p.areas.size() == 2);
		for (const ConquestAreaData &a : p.areas) {
			CHECK(a.params.time_to_return == 10);
			CHECK(a.params.team_specific);
			CHECK(a.params.ai_allowed_outside);
			CHECK(a.polygon.size() >= 3);
		}
		CHECK(p.areas[0].params.team != p.areas[1].params.team);
		const core::SnapshotDiff diff = core::diff_snapshots(mount->before, mount->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s\n", diff.clean() ? "clean" : diff.summary().c_str());
	}

	TEST_CASE("logic.conquest: MP_007 ConquestSmall0 through the retail graph: US takes every flag, RU bleeds out, the "
			  "graph ends the round for US") {
		test::Mounted *mount = test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		const std::optional<ConquestLevel> l = load_conquest(*mount, "MP_007", "ConquestSmall0");
		Match m = start_match(*l);
		const ConquestPointData *us_hq = hq_of(*l->params, 1);
		const ConquestPointData *ru_hq = hq_of(*l->params, 2);
		REQUIRE(us_hq != nullptr);
		REQUIRE(ru_hq != nullptr);
		REQUIRE(m.natives->player_enter_team(0, true, 1, *m.runtime).ok());
		REQUIRE(m.natives->player_enter_team(1, true, 2, *m.runtime).ok());
		std::vector<ConquestPlayerSample> players{ { 0, 1, true, true, us_hq->params.position },
			{ 1, 2, true, true, ru_hq->params.position } };
		REQUIRE(m.natives->player_spawned(0, 1, *m.runtime).ok());
		REQUIRE(m.natives->player_spawned(1, 2, *m.runtime).ok());
		for (int i = 0; i < 10 && m.natives->phase() != RoundPhase::InRound; ++i) {
			m.step(players);
		}
		m.step(players);
		REQUIRE(m.natives->phase() == RoundPhase::InRound);
		const ConquestNatives &c = *m.natives->conquest();
		CHECK(c.tickets(1) == 150);
		CHECK(c.tickets(2) == 150);
		CHECK(c.spawn_allowed(1));
		CHECK(c.spawn_allowed(2));
		for (const uint32_t s : us_hq->spawners) {
			if (!l->params->spawner(s)->vehicle) {
				CHECK(c.spawner_open(s, 1));
				CHECK_FALSE(c.spawner_open(s, 2));
			}
		}
		for (const ConquestCounterData &counter : l->params->counters) {
			const std::optional<LogicValue> shown = m.runtime->output(counter.node, logic_port_id("Tickets"));
			REQUIRE(shown.has_value());
			CHECK(shown->i == 150);
		}
		REQUIRE(m.natives->player_spawned(1, 2, *m.runtime).ok());
		CHECK(c.tickets(2) == 149);
		uint64_t capture_ticks_total = 0;
		for (const ConquestPointData &flag : l->params->points) {
			if (flag.params.type == CapturableType::UnableToChangeTeam) {
				continue;
			}
			for (const uint32_t s : flag.spawners) {
				CHECK_FALSE(c.spawner_open(s, 1));
				CHECK_FALSE(c.spawner_open(s, 2));
			}
			players[0].position = flag.params.position;
			const sim::Tick from = m.tick;
			bool taken = false;
			for (int i = 0; i < 1300 && !taken; ++i) {
				m.step(players);
				for (const ConquestPointView &v : c.points()) {
					taken |= v.node == flag.node && v.controlled && v.team == 1;
				}
			}
			REQUIRE(taken);
			capture_ticks_total += m.tick - from;
			CHECK(m.tick - from >= 1200);
			CHECK(m.tick - from <= 1202);
			size_t open_us = 0, open_ru = 0;
			for (const uint32_t s : flag.spawners) {
				if (!l->params->spawner(s)->vehicle) {
					open_us += c.spawner_open(s, 1) ? 1 : 0;
					open_ru += c.spawner_open(s, 2) ? 1 : 0;
				}
			}
			CHECK(open_us == 1);
			CHECK(open_ru == 0);
		}
		CHECK(c.stats().captures == 4);
		CHECK(c.tickets(1) == 150);
		CHECK(c.bleeding(2));
		const int64_t ru_before = c.tickets(2);
		for (int i = 0; i < 600; ++i) {
			m.step(players);
		}
		CHECK(double(ru_before - c.tickets(2)) == doctest::Approx(3.4).epsilon(0.35));
		CHECK(c.tickets(1) == 150);
		int guard = 0;
		while (m.natives->phase() == RoundPhase::InRound && guard < 60 * 600) {
			m.step(players);
			++guard;
		}
		std::printf("logic.conquest: 4 captures in %llu ticks, RU bled from %lld to %lld, round over after %d more "
					"ticks, winner %u, "
					"unhandled conquest inputs %llu\n",
				static_cast<unsigned long long>(capture_ticks_total), static_cast<long long>(ru_before),
				static_cast<long long>(c.tickets(2)), guard, unsigned(m.natives->winner()),
				static_cast<unsigned long long>(c.stats().unhandled_inputs));
		CHECK(m.natives->phase() == RoundPhase::RoundOver);
		CHECK(m.natives->winner() == 1);
		CHECK(c.tickets(2) == 0);
		for (const auto &[node, port] : c.stats().unhandled) {
			std::printf("logic.conquest: unhandled input port %d into node %u (%s)\n", port, node,
					std::string(logic_type_info(l->graph->nodes()[node].type).ebx_type).c_str());
		}
		CHECK(c.stats().unhandled_inputs == 0);
	}

	TEST_CASE("logic.conquest: MP_007 ConquestSmall0 opens only the HQ's spawn points at round start and a flag's "
			  "own-team points once taken") {
		test::Mounted *mount = test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		const std::optional<ConquestLevel> l = load_conquest(*mount, "MP_007", "ConquestSmall0");
		core::Result<core::level::LevelSpawns> spawns = core::level::read_level_spawns(*l->world);
		REQUIRE(spawns.ok());
		Match m = start_match(*l);
		const ConquestPointData *us_hq = hq_of(*l->params, 1);
		const ConquestPointData *ru_hq = hq_of(*l->params, 2);
		REQUIRE(us_hq != nullptr);
		REQUIRE(ru_hq != nullptr);
		REQUIRE(m.natives->player_enter_team(0, true, 1, *m.runtime).ok());
		REQUIRE(m.natives->player_enter_team(1, true, 2, *m.runtime).ok());
		std::vector<ConquestPlayerSample> players{ { 0, 1, true, true, us_hq->params.position },
			{ 1, 2, true, true, ru_hq->params.position } };
		m.step(players);
		const ConquestNatives &c = *m.natives->conquest();
		const auto open_points = [&](uint8_t p_team) {
			size_t n = 0;
			for (const core::level::SpawnPointDef &p : spawns->points) {
				n += c.spawn_point_open(p.spawner, p_team) ? 1 : 0;
			}
			return n;
		};
		const auto points_of = [&](const ConquestPointData &p_point, uint8_t p_team) {
			size_t n = 0;
			for (const uint32_t s : p_point.spawners) {
				const ConquestSpawnerData *sp = l->params->spawner(s);
				if (sp == nullptr || sp->vehicle || sp->gate.data_team != p_team) {
					continue;
				}
				for (const core::level::SpawnPointDef &pt : spawns->points) {
					n += pt.spawner == sp->level_object ? 1 : 0;
				}
			}
			return n;
		};
		size_t linked = 0;
		for (const core::level::SpawnPointDef &p : spawns->points) {
			linked += p.spawner != core::level::kNoSpawner ? 1 : 0;
		}
		CHECK(linked == spawns->points.size());
		const size_t us_start = open_points(1), ru_start = open_points(2);
		CHECK(us_start == points_of(*us_hq, 1));
		CHECK(ru_start == points_of(*ru_hq, 2));
		CHECK(us_start > 0);
		CHECK(ru_start > 0);
		const ConquestPointData *flag = nullptr;
		for (const ConquestPointData &p : l->params->points) {
			if (p.params.type != CapturableType::UnableToChangeTeam && flag == nullptr) {
				flag = &p;
			}
		}
		REQUIRE(flag != nullptr);
		players[0].position = flag->params.position;
		for (int i = 0; i < 1210; ++i) {
			m.step(players);
		}
		const size_t flag_us = points_of(*flag, 1);
		CHECK(flag_us > 0);
		CHECK(open_points(1) == us_start + flag_us);
		CHECK(open_points(2) == ru_start);
		std::printf(
				"logic.conquest: spawn points open at start US %zu RU %zu of %zu; the first flag adds %zu US points\n",
				us_start, ru_start, spawns->points.size(), flag_us);
	}
	TEST_CASE("logic.conquest: MP_007 ConquestSmall0 kills a US soldier 10 s after he leaves the US combat area") {
		test::Mounted *mount = test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		const std::optional<ConquestLevel> l = load_conquest(*mount, "MP_007", "ConquestSmall0");
		Match m = start_match(*l);
		const ConquestPointData *us_hq = hq_of(*l->params, 1);
		REQUIRE(us_hq != nullptr);
		REQUIRE(m.natives->player_enter_team(0, false, 1, *m.runtime).ok());
		REQUIRE(m.natives->player_enter_team(1, true, 2, *m.runtime).ok());
		std::vector<ConquestPlayerSample> players{ { 0, 1, true, false, us_hq->params.position },
			{ 1, 2, true, true, { 50000.0f, 0.0f, 50000.0f } } };
		for (int i = 0; i < 120; ++i) {
			m.step(players);
		}
		ConquestNatives &c = *m.natives->conquest();
		CHECK(c.take_kills().empty());
		CHECK(c.deserters(1).empty());
		players[0].position = { 50000.0f, 0.0f, 50000.0f };
		int killed_after = -1;
		for (int i = 1; i <= 700 && killed_after < 0; ++i) {
			m.step(players);
			const std::vector<uint8_t> kills = c.take_kills();
			if (!kills.empty()) {
				CHECK(kills.size() == 1);
				CHECK(kills.front() == 0);
				killed_after = i;
			}
		}
		CHECK(killed_after >= 599);
		CHECK(killed_after <= 601);
		CHECK(c.stats().strikes == 1);
	}
}

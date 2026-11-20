#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/placed_effects.h"
#include "bf3/core/level/world.h"
#include "bf3/host/ui_flow_session.h"
#include "bf3/sim/logic/conquest_data.h"
#include "bf3/sim/logic/under_fire.h"
#include "test_support.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace bf3;
namespace lvl = bf3::core::level;
namespace gp = bf3::core::gameplay;

namespace {

constexpr uint64_t kRadioTowerCollapse = 0x4b645d9fca970719ull;
constexpr const char *kTowerRuinSequence = "e35fb039";
constexpr const char *kTowerRuinEffect = "fx_spvalley_radiotowerimpact_l";

struct Opened {
	std::unique_ptr<lvl::LoadedLevel> level;
	std::unique_ptr<lvl::World> world;
	std::unique_ptr<host::UiFlowSession> session;
	host::UiGameState state;
};

std::optional<Opened> open(const char *p_level, const char *p_mode) {
	bf3::test::Mounted *mount = bf3::test::shared_mount_or_skip();
	if (mount == nullptr) {
		return std::nullopt;
	}
	Opened o;
	core::Result<std::unique_ptr<lvl::LoadedLevel>> loaded = lvl::load_level(mount->install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	o.level = std::move(*loaded);
	core::Result<std::unique_ptr<lvl::World>> world = lvl::World::traverse(*o.level);
	REQUIRE(world.ok());
	o.world = std::move(*world);
	host::UiFlowSessionOptions options;
	options.initialize_screens = false;
	core::Result<std::unique_ptr<host::UiFlowSession>> session = host::UiFlowSession::create(*o.level, options);
	REQUIRE_MESSAGE(session.ok(), (session.ok() ? std::string() : session.error().message));
	o.session = std::move(*session);
	o.state.joined = true;
	o.state.phase = net::MatchPhase::InRound;
	return o;
}

}

TEST_SUITE("logic-effects") {
	TEST_CASE("logic effects: Caspian's radio tower collapse starts its 14 placed effects on the client") {
		std::optional<Opened> o = open("MP_007", "ConquestLarge0");
		if (!o) {
			return;
		}
		const gp::LogicGraph &graph = o->session->runtime().graph();
		std::vector<uint32_t> targets;
		for (const gp::LogicEdge &e : graph.networked_edges()) {
			if (e.connection == kRadioTowerCollapse && e.target != gp::kNoLogicIndex) {
				targets.push_back(e.target);
			}
		}
		REQUIRE(targets.size() == 1);
		CHECK(graph.nodes()[targets[0]].type == gp::LogicTypeId::Sequence);

		core::Result<std::vector<lvl::PlacedEffect>> placed = lvl::read_placed_effects(*o->world);
		REQUIRE(placed.ok());
		std::map<uint32_t, size_t> placement_of;
		size_t bound = 0, waiting = 0;
		for (size_t i = 0; i < placed->size(); ++i) {
			const std::optional<uint32_t> node =
					sim::logic::placed_logic_node(graph, *o->world, (*placed)[i].object, gp::LogicTypeId::Effect);
			if (node) {
				placement_of[*node] = i;
				++bound;
			}
			waiting += (*placed)[i].auto_start ? 0 : 1;
		}

		const uint64_t cursor = o->session->effect_feed().end();
		sim::logic::LogicNetworkedEvent ev;
		ev.connection = kRadioTowerCollapse;
		o->session->receive_networked(std::span<const sim::logic::LogicNetworkedEvent>(&ev, 1));
		const sim::Tick first = o->session->runtime().tick();
		for (int t = 0; t < 31 * 60; ++t) {
			REQUIRE(o->session->update(o->state).ok());
		}
		std::set<uint32_t> started;
		std::vector<double> at;
		size_t unplaced = 0;
		for (const host::LogicEffectEvent &e : o->session->effect_feed().since(cursor)) {
			if (!e.start) {
				continue;
			}
			started.insert(e.node);
			at.push_back(double(e.tick - first) / double(sim::kSimHz));
			unplaced += placement_of.count(e.node) == 0 ? 1 : 0;
		}
		std::sort(at.begin(), at.end());
		std::printf(
				"logic.effects MP_007: %zu placed effects, %zu wait for logic, %zu bound to client logic nodes; the "
				"radio tower's networked Start reached %zu effect nodes, %zu Starts from %.2f s to %.2f s, %zu "
				"without a placement\n",
				placed->size(), waiting, bound, started.size(), at.size(), at.empty() ? 0.0 : at.front(),
				at.empty() ? 0.0 : at.back(), unplaced);
		CHECK(placed->size() == 338);
		CHECK(bound == placed->size());
		CHECK(placement_of.size() == bound);
		CHECK(started.size() == 14);
		CHECK(unplaced == 0);
		for (const uint32_t n : started) {
			CHECK_FALSE((*placed)[placement_of[n]].auto_start);
		}
		CHECK(o->session->errors().empty());
	}

	TEST_CASE("logic effects: Caspian's 99 waiting placed effects by what starts them, and every server-started one "
			  "starts") {
		std::optional<Opened> o = open("MP_007", "ConquestLarge0");
		if (!o) {
			return;
		}
		const gp::LogicGraph &graph = o->session->runtime().graph();
		std::map<uint32_t, std::vector<const gp::LogicEdge *>> into;
		for (const std::span<const gp::LogicEdge> list : { graph.event_edges(), graph.networked_edges() }) {
			for (const gp::LogicEdge &e : list) {
				if (e.target != gp::kNoLogicIndex) {
					into[e.target].push_back(&e);
				}
			}
		}
		core::Result<std::vector<lvl::PlacedEffect>> placed = lvl::read_placed_effects(*o->world);
		REQUIRE(placed.ok());
		core::Result<sim::logic::LogicGhostLayout> layout = sim::logic::LogicGhostLayout::of(graph);
		REQUIRE(layout.ok());
		const std::set<uint32_t> ghosted(layout->sequences.begin(), layout->sequences.end());
		std::set<uint32_t> entered;
		for (const gp::LogicEntry &en : graph.entries()) {
			entered.insert(en.target);
		}
		enum Driver { kServer, kUnderFire, kDestruction, kGhost, kEntry, kNone, kDeadEnd, kDrivers };
		const char *const names[kDrivers] = { "a server event", "an UnderFireTrigger", "a BreakableModel",
			"a sequence the server starts through the ghost", "a prefab input nothing on the client feeds",
			"no connection", "logic nothing starts" };
		size_t counts[kDrivers] = {};
		std::set<uint64_t> server_events;
		std::map<uint32_t, Driver> driver_of;
		for (const lvl::PlacedEffect &e : *placed) {
			if (e.auto_start) {
				continue;
			}
			const std::optional<uint32_t> node =
					sim::logic::placed_logic_node(graph, *o->world, e.object, gp::LogicTypeId::Effect);
			REQUIRE(node.has_value());
			std::set<uint32_t> seen;
			std::vector<uint32_t> open_nodes{ *node };
			bool server = false, under_fire = false, destruction = false, ghost = false, entry = false, any = false;
			std::set<uint64_t> connections;
			while (!open_nodes.empty() && seen.size() < 4096) {
				const uint32_t n = open_nodes.back();
				open_nodes.pop_back();
				if (!seen.insert(n).second) {
					continue;
				}
				const gp::LogicTypeId t = graph.nodes()[n].type;
				under_fire = under_fire || t == gp::LogicTypeId::UnderFireTrigger;
				destruction = destruction || t == gp::LogicTypeId::BreakableModel;
				ghost = ghost || ghosted.count(n) != 0;
				entry = entry || entered.count(n) != 0;
				for (const gp::LogicEdge *edge : into[n]) {
					any = true;
					if (edge->source == gp::kNoLogicIndex) {
						server = true;
						connections.insert(edge->connection);
					} else {
						open_nodes.push_back(edge->source);
					}
				}
			}
			const Driver d = server ? kServer
					: under_fire	? kUnderFire
					: destruction	? kDestruction
					: ghost			? kGhost
					: entry			? kEntry
					: !any			? kNone
									: kDeadEnd;
			++counts[d];
			driver_of[*node] = d;
			if (d == kServer) {
				server_events.insert(connections.begin(), connections.end());
			}
		}
		std::string line = "logic.effects MP_007 waiting placed effects started by:";
		size_t total = 0;
		for (size_t d = 0; d < kDrivers; ++d) {
			line += std::string(" ") + names[d] + " " + std::to_string(counts[d]) + ";";
			total += counts[d];
		}
		const uint64_t cursor = o->session->effect_feed().end();
		std::vector<sim::logic::LogicNetworkedEvent> events;
		for (const uint64_t c : server_events) {
			sim::logic::LogicNetworkedEvent ev;
			ev.connection = c;
			events.push_back(ev);
		}
		o->session->receive_networked(events);
		for (int t = 0; t < 60 * 60; ++t) {
			REQUIRE(o->session->update(o->state).ok());
		}
		std::set<uint32_t> started;
		for (const host::LogicEffectEvent &e : o->session->effect_feed().since(cursor)) {
			if (e.start && driver_of.count(e.node) != 0 && driver_of[e.node] == kServer) {
				started.insert(e.node);
			}
		}
		std::printf("%s %zu server events delivered start %zu of them\n", line.c_str(), server_events.size(),
				started.size());
		CHECK(total == 99);
		CHECK(counts[kServer] == 22);
		CHECK(counts[kUnderFire] == 10);
		CHECK(counts[kDestruction] == 40);
		CHECK(counts[kGhost] == 6);
		CHECK(counts[kEntry] == 3);
		CHECK(counts[kNone] == 4);
		CHECK(counts[kDeadEnd] == 14);
		CHECK(started.size() == counts[kServer]);
		CHECK(o->session->errors().empty());
	}

	TEST_CASE("logic effects: a server-started sequence the client saw start runs its OnStarted, so the radio tower "
			  "ruin's 6 effects start; a late joiner's first ghost starts none") {
		const auto run = [](bool p_live) -> size_t {
			std::optional<Opened> o = open("MP_007", "ConquestLarge0");
			if (!o) {
				return 0;
			}
			const gp::LogicGraph &graph = o->session->runtime().graph();
			std::optional<uint32_t> ruin;
			for (const uint32_t n : graph.nodes_of_type(gp::LogicTypeId::Sequence)) {
				if (graph.nodes()[n].guid.to_string().starts_with(kTowerRuinSequence)) {
					ruin = n;
				}
			}
			REQUIRE(ruin.has_value());
			core::Result<sim::logic::LogicGhostLayout> layout = sim::logic::LogicGhostLayout::of(graph);
			REQUIRE(layout.ok());
			const auto at = std::find(layout->sequences.begin(), layout->sequences.end(), *ruin);
			REQUIRE(at != layout->sequences.end());
			const size_t index = size_t(at - layout->sequences.begin());
			REQUIRE(o->session->update(o->state).ok());
			if (p_live) {
				REQUIRE(o->session->apply_ghost(o->session->runtime().ghost()).ok());
				REQUIRE(o->session->update(o->state).ok());
			}
			const uint64_t cursor = o->session->effect_feed().end();
			sim::logic::LogicGhost started = o->session->runtime().ghost();
			started.sequence_start[index] = started.tick;
			REQUIRE(o->session->apply_ghost(started).ok());
			for (int t = 0; t < 45 * 60; ++t) {
				REQUIRE(o->session->update(o->state).ok());
			}
			core::Result<std::vector<lvl::PlacedEffect>> placed = lvl::read_placed_effects(*o->world);
			REQUIRE(placed.ok());
			std::set<uint32_t> ruin_effects;
			for (const lvl::PlacedEffect &e : *placed) {
				if (e.blueprint.ends_with(kTowerRuinEffect)) {
					const std::optional<uint32_t> node =
							sim::logic::placed_logic_node(graph, *o->world, e.object, gp::LogicTypeId::Effect);
					REQUIRE(node.has_value());
					ruin_effects.insert(*node);
				}
			}
			CHECK(ruin_effects.size() == 6);
			std::set<uint32_t> fired;
			for (const host::LogicEffectEvent &e : o->session->effect_feed().since(cursor)) {
				if (e.start && ruin_effects.count(e.node) != 0) {
					fired.insert(e.node);
				}
			}
			CHECK(o->session->errors().empty());
			return fired.size();
		};
		if (bf3::test::shared_mount_or_skip() == nullptr) {
			return;
		}
		const size_t live = run(true);
		const size_t late = run(false);
		std::printf("logic.effects MP_007: the tower ruin sequence started by the server: %zu of 6 effects start on a "
					"client that saw it start, %zu on a late joiner\n",
				live, late);
		CHECK(live == 6);
		CHECK(late == 0);
	}
	TEST_CASE("logic effects: a hit inside an UnderFireTrigger's Radius fires it on the server once, and its networked "
			  "Start starts the 10 placed effects behind it on the client; a hit outside starts none") {
		std::optional<Opened> o = open("MP_007", "ConquestLarge0");
		if (!o) {
			return;
		}
		gp::LogicGraphOptions go;
		go.mode = o->level->mode;
		go.side = gp::LogicSide::Server;
		gp::LogicWorldOptions wo;
		wo.mode = go.mode;
		wo.expand_root_objects = true;
		core::Result<gp::LogicWorld> world = gp::LogicWorld::build(o->level->db(), o->level->root(), wo);
		REQUIRE(world.ok());
		core::Result<gp::LogicGraph> built = gp::build_logic_graph(*world, go);
		REQUIRE(built.ok());
		auto server_graph = std::make_shared<const gp::LogicGraph>(std::move(*built));
		core::Result<std::shared_ptr<const sim::logic::UnderFireParams>> params =
				sim::logic::read_under_fire_params(*server_graph, *o->world);
		REQUIRE(params.ok());
		REQUIRE_FALSE((*params)->triggers.empty());
		for (const std::string &n : (*params)->notes) {
			MESSAGE(n);
		}

		const gp::LogicGraph &client = o->session->runtime().graph();
		core::Result<std::vector<lvl::PlacedEffect>> placed = lvl::read_placed_effects(*o->world);
		REQUIRE(placed.ok());
		std::set<uint32_t> waiting;
		for (const lvl::PlacedEffect &e : *placed) {
			if (!e.auto_start) {
				const std::optional<uint32_t> node =
						sim::logic::placed_logic_node(client, *o->world, e.object, gp::LogicTypeId::Effect);
				REQUIRE(node.has_value());
				waiting.insert(*node);
			}
		}

		const auto shoot = [&](bool p_inside) -> std::pair<size_t, sim::logic::UnderFireStats> {
			sim::logic::LogicRuntimeOptions ro;
			core::Result<sim::logic::LogicRuntime> rt = sim::logic::LogicRuntime::create(server_graph, ro);
			REQUIRE(rt.ok());
			REQUIRE(rt->start().ok());
			sim::logic::UnderFireNatives natives(*params);
			std::vector<sim::logic::LogicNetworkedEvent> sent;
			for (int t = 0; t < 5 * 60; ++t) {
				const sim::Tick next = rt->tick() + 1u;
				natives.apply(next, rt->effects());
				std::vector<core::Vec3> points;
				if (t == 30 || t == 31 || t == 4 * 60) {
					for (const sim::logic::UnderFireTriggerData &d : (*params)->triggers) {
						core::Vec3 p = d.position;
						p.y += p_inside ? d.radius * 0.5f : d.radius * 1.5f;
						points.push_back(p);
					}
				}
				REQUIRE(natives.impacts(next, points, *rt).ok());
				REQUIRE(rt->step().ok());
				for (const sim::logic::LogicNetworkedEvent &e : rt->networked()) {
					sent.push_back(e);
				}
			}
			const uint64_t cursor = o->session->effect_feed().end();
			o->session->receive_networked(sent);
			for (int t = 0; t < 30 * 60; ++t) {
				REQUIRE(o->session->update(o->state).ok());
			}
			std::set<uint32_t> started;
			for (const host::LogicEffectEvent &e : o->session->effect_feed().since(cursor)) {
				if (e.start && waiting.count(e.node) != 0) {
					started.insert(e.node);
				}
			}
			return { started.size(), natives.stats() };
		};
		const auto [outside, out_stats] = shoot(false);
		const auto [inside, in_stats] = shoot(true);
		std::printf("logic.effects MP_007: %zu UnderFireTriggers (radius %.1f, cool down %.1f s, run once %d); hits "
					"inside: fired %llu, %llu inside, %llu spent, %zu waiting effects started; hits outside: fired "
					"%llu, %zu started\n",
				(*params)->triggers.size(), double((*params)->triggers[0].radius),
				double((*params)->triggers[0].cool_down_s), int((*params)->triggers[0].run_once),
				(unsigned long long)in_stats.fired, (unsigned long long)in_stats.inside,
				(unsigned long long)in_stats.spent, inside, (unsigned long long)out_stats.fired, outside);
		CHECK(outside == 0);
		CHECK(out_stats.fired == 0);
		CHECK(in_stats.fired == (*params)->triggers.size());
		CHECK(inside == 10);
		CHECK(o->session->errors().empty());
	}
}

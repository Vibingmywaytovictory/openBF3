#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/placed_effects.h"
#include "bf3/core/level/world.h"
#include "bf3/host/ui_flow_session.h"
#include "bf3/sim/logic/conquest_data.h"
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
		enum Driver { kServer, kUnderFire, kDestruction, kNone, kOther, kDrivers };
		const char *const names[kDrivers] = { "a server event", "an UnderFireTrigger", "a BreakableModel", "nothing",
			"other client logic" };
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
			bool server = false, under_fire = false, destruction = false, any = false;
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
					: !any			? kNone
									: kOther;
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
		CHECK(started.size() == counts[kServer]);
		CHECK(o->session->errors().empty());
	}
}

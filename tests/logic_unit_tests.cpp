// logic.unit (docs/design/s4.md §12): the S4 level-logic interpreter and graph builder on hand-made graphs, no data.
//   * Builder graphs: FIFO and fan-out order, Delay ticks (0, 0.1 s, 10 s), RemoveDuplicateEvents, the per-tick
//     delivery cap and carry-over (tick 0 included), queue overflow faults, native effects at t + 1, the init settle,
//     BoolToEvent InitalEvent polarity, DelayTrigger Enable/Disable, MultipleTrigger with a Delay, watches, networked
//     events, RandomDelay seeding, the ghost (round trip, delta, hostile bytes, the 4,096 cap) and a pinned trace hash;
//   * synthetic EBX (tests/logic_test_support.h) through build_logic_graph: interface flattening (in, out,
//     pass-through, nested two levels), an unbound reference, a Client-realm node, the TargetType filter, a diamond
//     through two interfaces, an interface cycle, connected vs unconnected unknown types, connection ids equal on a
//     server and a client build, and find_nodes by nested scope reference.
#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "logic_test_support.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;
using namespace bf3::sim::logic;
using bf3::sim::Tick;
using logic_test::SynArray;
using logic_test::SynInstance;
using logic_test::SynPartition;
using logic_test::SynType;
using logic_test::SynValues;

namespace {

namespace port = logic_port;
using K = LogicValue::Kind;

LogicValue v_bool(bool p_b) {
	LogicValue v;
	v.kind = K::Bool;
	v.b = p_b;
	return v;
}

LogicValue v_int(int64_t p_i) {
	LogicValue v;
	v.kind = K::Int;
	v.i = p_i;
	return v;
}

LogicValue v_float(double p_f) {
	LogicValue v;
	v.kind = K::Float;
	v.f = p_f;
	return v;
}

Guid guid_n(uint32_t p_n) {
	Guid g{};
	std::memcpy(g.bytes.data(), &p_n, 4);
	g.bytes[15] = 0x54;
	return g;
}

// A Builder with one root scope; nodes get distinct GUIDs.
struct G {
	LogicGraph::Builder b;
	uint32_t root = 0;
	uint32_t next = 1;
	explicit G(LogicSide p_side = LogicSide::Server) :
			b(p_side) {
		root = b.add_scope(kNoLogicIndex, Guid{}, "root");
	}
	uint32_t node(LogicTypeId p_type, LogicRealm p_realm = LogicRealm::Server) { return b.add_node(p_type, p_realm, root, guid_n(next++)); }
	std::shared_ptr<const LogicGraph> finish(const LogicGraphOptions &p_caps = {}) {
		Result<LogicGraph> g = b.finish(p_caps);
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		return std::make_shared<const LogicGraph>(std::move(*g));
	}
};

LogicRuntime make_runtime(std::shared_ptr<const LogicGraph> p_graph, LogicRuntimeOptions p_options = {}) {
	p_options.keep_trace_records = true;
	Result<LogicRuntime> rt = LogicRuntime::create(std::move(p_graph), p_options);
	REQUIRE_MESSAGE(rt.ok(), (rt.ok() ? std::string() : rt.error().message));
	return std::move(*rt);
}

void start(LogicRuntime &r_rt) {
	Result<LogicTickStats> s = r_rt.start();
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
}

LogicTickStats step(LogicRuntime &r_rt) {
	Result<LogicTickStats> s = r_rt.step();
	REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	return *s;
}

// Event deliveries (trace records) into p_target, as (tick, port).
std::vector<std::pair<Tick, int32_t>> events_into(const LogicRuntime &p_rt, uint32_t p_target) {
	std::vector<std::pair<Tick, int32_t>> out;
	for (const LogicTraceRecord &r : p_rt.trace().records) {
		if (r.kind == LogicTraceKind::Event && r.target == p_target) {
			out.emplace_back(r.tick, r.target_port);
		}
	}
	return out;
}

std::vector<Tick> ticks_into(const LogicRuntime &p_rt, uint32_t p_target, int32_t p_port) {
	std::vector<Tick> out;
	for (const auto &[tick, p] : events_into(p_rt, p_target)) {
		if (p == p_port) {
			out.push_back(tick);
		}
	}
	return out;
}

// --- synthetic EBX -------------------------------------------------------------------------------------------------

constexpr const char *kRealms[] = { "Realm_Client", "Realm_Server", "Realm_ClientAndServer", "Realm_None", "Realm_Pipeline" };
constexpr const char *kTargets[] = { "EventConnectionTargetType_Invalid", "EventConnectionTargetType_ClientAndServer",
	"EventConnectionTargetType_Client", "EventConnectionTargetType_Server", "EventConnectionTargetType_NetworkedClient",
	"EventConnectionTargetType_NetworkedClientAndServer" };

uint32_t realm_word(LogicRealm p_realm) {
	return static_cast<uint32_t>(p_realm) + 1; // enumerators are valued 1, 2, ... in SynPartition
}

uint32_t target_word(LogicEventTarget p_target) {
	return static_cast<uint32_t>(p_target) + 1;
}

// A partition of prefab blueprints (instance 0 is the root prefab) with entities, child references, interface
// descriptors and the three connection arrays.
struct LogicSyn {
	SynPartition s;
	struct Prefab {
		uint32_t instance = 0;
		std::vector<uint32_t> objects; // refs
		std::vector<SynValues> events, properties, links;
		std::optional<uint32_t> descriptor;
	};
	std::vector<Prefab> prefabs;

	LogicSyn() {
		SynType realm{ "Realm", {}, false, {}, false, true };
		for (const char *r : kRealms) {
			realm.fields.push_back({ r, 'i', {} });
		}
		SynType target{ "EventConnectionTargetType", {}, false, {}, false, true };
		for (const char *t : kTargets) {
			target.fields.push_back({ t, 'i', {} });
		}
		s.types = {
			realm,
			target,
			{ "EventSpec", { { "Id", 'i' } }, true },
			{ "EventConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceEvent", 'S', "EventSpec" }, { "TargetEvent", 'S', "EventSpec" },
										 { "TargetType", 'e', "EventConnectionTargetType" } },
					true },
			{ "PropertyConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceFieldId", 'i' }, { "TargetFieldId", 'i' } }, true },
			{ "LinkConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceFieldId", 'i' }, { "TargetFieldId", 'i' } }, true },
			{ "DataField", { { "Id", 'i' }, { "Value", 's' } }, true },
			{ "InterfaceDescriptorData", { { "Fields", 'v', "DataField" } } },
			{ "PrefabBlueprint", { { "Objects", 'a' }, { "EventConnections", 'v', "EventConnection" }, { "PropertyConnections", 'v', "PropertyConnection" },
										 { "LinkConnections", 'v', "LinkConnection" }, { "Descriptor", 'r' } } },
			{ "ReferenceObjectData", { { "Blueprint", 'r' }, { "Excluded", 'b' } } },
			{ "EventSplitterEntityData", { { "Realm", 'e', "Realm" }, { "RunOnce", 'b' } } },
			{ "EventGateEntityData", { { "Realm", 'e', "Realm" }, { "Default", 'b' } } },
			{ "DelayEntityData", { { "Realm", 'e', "Realm" }, { "Delay", 'f' }, { "AutoStart", 'b' }, { "RunOnce", 'b' } } },
			{ "BoolEntityData", { { "Realm", 'e', "Realm" }, { "DefaultValue", 'b' } } },
			{ "NotEntityData", { { "Realm", 'e', "Realm" }, { "In", 'b' } } },
			{ "MysteryEntityData", { { "Realm", 'e', "Realm" } } },
		};
		add_prefab(); // the root
	}

	uint32_t add_prefab() {
		Prefab p;
		p.instance = s.add({ "PrefabBlueprint", {} });
		prefabs.push_back(p);
		return static_cast<uint32_t>(prefabs.size() - 1);
	}
	// An interface descriptor for prefab p_prefab; returns its instance.
	uint32_t descriptor(uint32_t p_prefab) {
		if (!prefabs[p_prefab].descriptor) {
			prefabs[p_prefab].descriptor = s.add({ "InterfaceDescriptorData", {} });
		}
		return *prefabs[p_prefab].descriptor;
	}
	uint32_t entity(uint32_t p_prefab, const char *p_type, LogicRealm p_realm, SynValues p_values = {}) {
		p_values.push_back({ "Realm", realm_word(p_realm) });
		const uint32_t i = s.add({ p_type, std::move(p_values) });
		prefabs[p_prefab].objects.push_back(SynPartition::ref(i));
		return i;
	}
	// A reference in p_prefab instantiating p_child (or nothing, when p_child is negative).
	uint32_t reference(uint32_t p_prefab, int p_child) {
		SynValues v;
		if (p_child >= 0) {
			v.push_back({ "Blueprint", SynPartition::ref(prefabs[static_cast<size_t>(p_child)].instance) });
		}
		const uint32_t i = s.add({ "ReferenceObjectData", v });
		prefabs[p_prefab].objects.push_back(SynPartition::ref(i));
		return i;
	}
	void event(uint32_t p_prefab, uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port,
			LogicEventTarget p_tt = LogicEventTarget::Server) {
		prefabs[p_prefab].events.push_back({ { "Source", SynPartition::ref(p_source) }, { "Target", SynPartition::ref(p_target) },
				{ "SourceEvent", static_cast<uint32_t>(p_source_port) }, { "TargetEvent", static_cast<uint32_t>(p_target_port) },
				{ "TargetType", target_word(p_tt) } });
	}
	void property(uint32_t p_prefab, uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port) {
		prefabs[p_prefab].properties.push_back({ { "Source", SynPartition::ref(p_source) }, { "Target", SynPartition::ref(p_target) },
				{ "SourceFieldId", static_cast<uint32_t>(p_source_port) }, { "TargetFieldId", static_cast<uint32_t>(p_target_port) } });
	}

	// Builds and inserts the partition, then the graph of its root prefab.
	Result<LogicGraph> build(EbxDb &r_db, LogicSide p_side = LogicSide::Server, LogicGraphOptions p_options = {}) {
		for (const Prefab &p : prefabs) {
			SynValues v{ { "Objects", s.add_array(SynArray{ {}, p.objects, {} }) } };
			if (!p.events.empty()) {
				v.push_back({ "EventConnections", s.add_array(SynArray{ "EventConnection", {}, p.events }) });
			}
			if (!p.properties.empty()) {
				v.push_back({ "PropertyConnections", s.add_array(SynArray{ "PropertyConnection", {}, p.properties }) });
			}
			if (!p.links.empty()) {
				v.push_back({ "LinkConnections", s.add_array(SynArray{ "LinkConnection", {}, p.links }) });
			}
			if (p.descriptor) {
				v.push_back({ "Descriptor", SynPartition::ref(*p.descriptor) });
			}
			s.instances[p.instance].values = v;
		}
		const EbxPartition *part = logic_test::insert_syn(r_db, "levels/synthetic/logic", s);
		if (part == nullptr) {
			return make_error("synthetic partition did not parse");
		}
		p_options.side = p_side;
		return build_logic_graph(r_db, part->primary(), p_options);
	}
};

// Index of the node with that EBX instance (GUID from SynPartition::guid); kNoLogicIndex when absent.
uint32_t node_of(const LogicGraph &p_graph, const SynPartition &p_syn, uint32_t p_instance) {
	const std::span<const uint32_t> found = p_graph.nodes_with_guid(p_syn.guid(p_instance));
	return found.empty() ? kNoLogicIndex : found[0];
}

bool has_event(const LogicGraph &p_graph, uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port) {
	for (const LogicEdge &e : p_graph.event_edges()) {
		if (e.source == p_source && e.source_port == p_source_port && e.target == p_target && e.target_port == p_target_port) {
			return true;
		}
	}
	return false;
}

constexpr int32_t kX = 1001, kY = 1002, kX2 = 1003, kY2 = 1004;

} // namespace

TEST_SUITE("logic-unit") {
	TEST_CASE("logic: FIFO and fan-out order") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t b1 = g.node(LogicTypeId::EventGate), b2 = g.node(LogicTypeId::EventGate);
		const uint32_t c1 = g.node(LogicTypeId::EventGate), c2 = g.node(LogicTypeId::EventGate);
		g.b.add_event(s, port::kSplitterOut, b1, port::kIn);
		g.b.add_event(s, port::kSplitterOut, b2, port::kIn);
		g.b.add_event(b1, port::kOut, c1, port::kIn);
		g.b.add_event(b2, port::kOut, c2, port::kIn);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
		const LogicTickStats st = step(rt);
		CHECK(st.deliveries == 4);
		std::vector<uint32_t> order;
		for (const LogicTraceRecord &r : rt.trace().records) {
			if (r.kind == LogicTraceKind::Event) {
				order.push_back(r.target);
			}
		}
		CHECK(order == std::vector<uint32_t>{ b1, b2, c1, c2 }); // breadth-first: the queue is FIFO
	}

	TEST_CASE("logic: Delay ticks (0 same drain, 0.1 s -> 6, 10 s -> 600) and RemoveDuplicateEvents") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const double delays[] = { 0.0, static_cast<double>(0.1f), 10.0 };
		uint32_t sinks[3] = {};
		for (int k = 0; k < 3; ++k) {
			const uint32_t d = g.node(LogicTypeId::Delay);
			g.b.add_param(d, port::kDelay, v_float(delays[k]));
			sinks[k] = g.node(LogicTypeId::EventGate);
			g.b.add_event(s, port::kSplitterOut, d, port::kIn);
			g.b.add_event(d, port::kOut, sinks[k], port::kIn);
		}
		// RemoveDuplicateEvents: a second In while pending is dropped.
		const uint32_t dup = g.node(LogicTypeId::Delay);
		g.b.add_param(dup, port::kDelay, v_float(1.0));
		g.b.add_param(dup, logic_port_id("RemoveDuplicateEvents"), v_bool(true));
		const uint32_t dup_sink = g.node(LogicTypeId::EventGate);
		g.b.add_event(s, port::kSplitterOut, dup, port::kIn);
		g.b.add_event(dup, port::kOut, dup_sink, port::kIn);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (Tick t = 1; t <= 700; ++t) {
			if (t == 1 || t == 2) {
				REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
			}
			step(rt);
		}
		CHECK(ticks_into(rt, sinks[0], port::kIn) == std::vector<Tick>{ 1, 2 });
		CHECK(ticks_into(rt, sinks[1], port::kIn) == std::vector<Tick>{ 7, 8 });
		CHECK(ticks_into(rt, sinks[2], port::kIn) == std::vector<Tick>{ 601, 602 });
		CHECK(ticks_into(rt, dup_sink, port::kIn) == std::vector<Tick>{ 61 });
	}

	TEST_CASE("logic: the delivery cap carries over in order, tick 0 included") {
		constexpr uint32_t kFan = 12000;
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t sink = g.node(LogicTypeId::EventGate);
		g.b.add_param(sink, logic_port_id("Default"), v_bool(false));
		for (uint32_t k = 0; k < kFan; ++k) {
			g.b.add_event(s, port::kSplitterOut, sink, port::kIn);
		}
		// Tick 0: a Bool publishes its initial value along kFan property edges.
		const uint32_t b = g.node(LogicTypeId::Bool);
		g.b.add_param(b, logic_port_id("DefaultValue"), v_bool(true));
		const uint32_t n = g.node(LogicTypeId::Not);
		for (uint32_t k = 0; k < kFan; ++k) {
			g.b.add_property(b, port::kValue, n, port::kIn);
		}
		LogicRuntime rt = make_runtime(g.finish());
		Result<LogicTickStats> t0 = rt.start();
		REQUIRE(t0.ok());
		CHECK(t0->deliveries == 10000);
		CHECK(t0->carried_over == kFan - 10000);
		LogicTickStats t1 = step(rt);
		CHECK(t1.deliveries == kFan - 10000);
		CHECK(t1.carried_over == 0);
		REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
		const LogicTickStats t2 = step(rt);
		CHECK(t2.deliveries == 10000);
		CHECK(t2.carried_over == kFan - 10000);
		const LogicTickStats t3 = step(rt);
		CHECK(t3.deliveries == kFan - 10000);
		CHECK(t3.carried_over == 0);
		size_t carried_records = 0;
		for (const LogicTraceRecord &r : rt.trace().records) {
			carried_records += r.kind == LogicTraceKind::CarriedOver ? 1 : 0;
		}
		CHECK(carried_records == 2);
	}

	TEST_CASE("logic: queue overflow faults the runtime, never grows past the cap") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t sink = g.node(LogicTypeId::EventGate);
		for (int k = 0; k < 200; ++k) {
			g.b.add_event(s, port::kSplitterOut, sink, port::kIn);
		}
		LogicRuntimeOptions o;
		o.max_queue = 100;
		LogicRuntime rt = make_runtime(g.finish(), o);
		start(rt);
		REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
		Result<LogicTickStats> st = rt.step();
		CHECK_FALSE(st.ok());
		CHECK(rt.faulted());
		REQUIRE(rt.fault() != nullptr);
		CHECK_FALSE(rt.step().ok()); // stays faulted
	}

	TEST_CASE("logic: effects on natives apply at the next tick") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t spawner = g.node(LogicTypeId::CharacterSpawn);
		const uint32_t b = g.node(LogicTypeId::Bool);
		g.b.add_event(s, port::kSplitterOut, spawner, port::kEnable);
		g.b.add_event(s, port::kSplitterOut, b, port::kSetTrue);
		g.b.add_property(b, port::kValue, spawner, port::kEnabled);
		std::shared_ptr<const LogicGraph> graph = g.finish();
		LogicRuntime rt = make_runtime(graph);
		Result<LogicNativeStubs> stubs = LogicNativeStubs::create(graph);
		REQUIRE(stubs.ok());
		start(rt);
		// Tick 0: the Bool's initial false reaches the spawner as an effect for tick 1.
		REQUIRE(rt.effects().size() == 1);
		CHECK(rt.effects()[0].apply_tick == 1);
		CHECK(rt.effects()[0].kind == LogicEffectKind::Property);
		std::vector<LogicEffect> pending(rt.effects().begin(), rt.effects().end());
		for (Tick t = 1; t <= 5; ++t) {
			REQUIRE(stubs->apply(t, pending, rt).ok());
			if (t == 3) {
				REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
			}
			step(rt);
			if (t == 3) {
				REQUIRE(rt.effects().size() == 2);
				for (const LogicEffect &e : rt.effects()) {
					CHECK(e.apply_tick == 4);
					CHECK(e.node == spawner);
				}
			}
			pending.assign(rt.effects().begin(), rt.effects().end());
		}
		std::vector<std::pair<Tick, int64_t>> enabled;
		for (const NativeTimelineEntry &e : stubs->timeline()) {
			if (e.node == spawner && e.what == NativeTimelineEntry::What::Enabled) {
				enabled.emplace_back(e.tick, e.value);
			}
		}
		// A native without an Enabled field starts enabled; the Bool's initial false applies at 1, the SetTrue at 4.
		CHECK(enabled == std::vector<std::pair<Tick, int64_t>>{ { 1, 0 }, { 4, 1 } });
	}

	TEST_CASE("logic: init settle is silent, then InitalEvent fires the event matching the settled value") {
		for (const bool initial : { false, true }) {
			CAPTURE(initial);
			G g;
			const uint32_t b = g.node(LogicTypeId::Bool);
			g.b.add_param(b, logic_port_id("DefaultValue"), v_bool(initial));
			const uint32_t be = g.node(LogicTypeId::BoolToEvent);
			g.b.add_param(be, logic_port_id("InitalEvent"), v_bool(true));
			g.b.add_property(b, port::kValue, be, port::kValue);
			const uint32_t on_true = g.node(LogicTypeId::EventGate), on_false = g.node(LogicTypeId::EventGate);
			g.b.add_event(be, port::kOnTrue, on_true, port::kIn);
			g.b.add_event(be, port::kOnFalse, on_false, port::kIn);
			// A SyncedBool whose initial In differs from its param must not fire OnTrue/OnFalse while settling.
			const uint32_t sb = g.node(LogicTypeId::SyncedBool);
			g.b.add_input(sb, port::kIn, v_bool(true));
			const uint32_t sb_sink = g.node(LogicTypeId::EventGate);
			g.b.add_event(sb, port::kOnTrue, sb_sink, port::kIn);
			LogicRuntime rt = make_runtime(g.finish());
			start(rt);
			CHECK(ticks_into(rt, on_true, port::kIn).size() == (initial ? 1u : 0u));
			CHECK(ticks_into(rt, on_false, port::kIn).size() == (initial ? 0u : 1u));
			CHECK(ticks_into(rt, sb_sink, port::kIn).empty());
			CHECK(rt.output(sb, port::kOut).has_value());
			CHECK(rt.output(sb, port::kOut)->b);
		}
	}

	TEST_CASE("logic: DelayTrigger Enable/Disable and MultipleTrigger with a Delay") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t enable = g.node(LogicTypeId::EventSplitter), disable = g.node(LogicTypeId::EventSplitter);
		const uint32_t dt = g.node(LogicTypeId::DelayTrigger);
		g.b.add_param(dt, port::kEnabled, v_bool(false));
		g.b.add_param(dt, port::kDelay, v_float(0.0));
		const uint32_t dt_sink = g.node(LogicTypeId::EventGate);
		g.b.add_event(s, port::kSplitterOut, dt, port::kTrigger);
		g.b.add_event(enable, port::kSplitterOut, dt, port::kEnable);
		g.b.add_event(disable, port::kSplitterOut, dt, port::kDisable);
		g.b.add_event(dt, port::kOnTrigger, dt_sink, port::kIn);
		const uint32_t mt = g.node(LogicTypeId::MultipleTrigger);
		g.b.add_param(mt, logic_port_id("TriggerAmount"), v_int(2));
		g.b.add_param(mt, port::kDelay, v_float(0.5));
		g.b.add_param(mt, logic_port_id("RunOnce"), v_bool(true));
		const uint32_t mt_sink = g.node(LogicTypeId::EventGate);
		g.b.add_event(s, port::kSplitterOut, mt, port::kTrigger);
		g.b.add_event(mt, port::kOnTrigger, mt_sink, port::kIn);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (Tick t = 1; t <= 100; ++t) {
			if (t == 1 || t == 3 || t == 5 || t == 7) {
				REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
			}
			if (t == 2) {
				REQUIRE(rt.force_event(enable, port::kSplitterOut).ok());
			}
			if (t == 4) {
				REQUIRE(rt.force_event(disable, port::kSplitterOut).ok());
			}
			step(rt);
		}
		CHECK(ticks_into(rt, dt_sink, port::kIn) == std::vector<Tick>{ 3 }); // disabled at 1 and 5
		CHECK(ticks_into(rt, mt_sink, port::kIn) == std::vector<Tick>{ 3 + 30 }); // 2nd trigger at 3, 0.5 s, RunOnce
	}

	TEST_CASE("logic: watches - EventIn per delivery, EventOut once per firing") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t sink = g.node(LogicTypeId::EventGate);
		g.b.add_event(s, port::kSplitterOut, sink, port::kIn);
		g.b.add_event(s, port::kSplitterOut, sink, port::kIn);
		LogicRuntime rt = make_runtime(g.finish());
		REQUIRE(rt.watch(sink, port::kIn, LogicWatchKind::EventIn).ok());
		REQUIRE(rt.watch(s, port::kSplitterOut, LogicWatchKind::EventOut).ok());
		CHECK_FALSE(rt.watch(999, port::kIn, LogicWatchKind::EventIn).ok());
		start(rt);
		REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
		step(rt);
		size_t in = 0, out = 0;
		for (const LogicWatchHit &h : rt.watched()) {
			in += h.kind == LogicWatchKind::EventIn ? 1 : 0;
			out += h.kind == LogicWatchKind::EventOut ? 1 : 0;
			CHECK(h.tick == 1);
		}
		CHECK(in == 2);
		CHECK(out == 1);
	}

	TEST_CASE("logic: networked events carry the connection id") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t sink = g.node(LogicTypeId::EventGate);
		g.b.add_event(s, port::kSplitterOut, sink, port::kIn, LogicEventTarget::NetworkedClientAndServer, 0xabcdef);
		g.b.add_networked_only(s, port::kSplitterOut, kNoLogicIndex, port::kIn, LogicEventTarget::NetworkedClient, 0x1234);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		REQUIRE(rt.force_event(s, port::kSplitterOut, LogicContext{ 3, 1 }).ok());
		const LogicTickStats st = step(rt);
		CHECK(st.deliveries == 1); // the networked-only edge is fired for clients, not delivered
		REQUIRE(rt.networked().size() == 2);
		std::vector<uint64_t> ids;
		for (const LogicNetworkedEvent &e : rt.networked()) {
			ids.push_back(e.connection);
			CHECK(e.tick == 1);
			CHECK(e.context.player == 3);
		}
		std::sort(ids.begin(), ids.end());
		CHECK(ids == std::vector<uint64_t>{ 0x1234, 0xabcdef });
	}

	TEST_CASE("logic: RandomDelay draws from the root seed only") {
		const auto fire_ticks = [](uint64_t p_seed) {
			G g;
			const uint32_t s = g.node(LogicTypeId::EventSplitter);
			std::vector<uint32_t> sinks;
			for (int k = 0; k < 8; ++k) {
				const uint32_t d = g.node(LogicTypeId::RandomDelay);
				g.b.add_param(d, logic_port_id("MinDelay"), v_float(1.0));
				g.b.add_param(d, logic_port_id("MaxDelay"), v_float(5.0));
				const uint32_t sink = g.node(LogicTypeId::EventGate);
				g.b.add_event(s, port::kSplitterOut, d, port::kIn);
				g.b.add_event(d, port::kOut, sink, port::kIn);
				sinks.push_back(sink);
			}
			LogicRuntimeOptions o;
			o.root_seed = p_seed;
			LogicRuntime rt = make_runtime(g.finish(), o);
			start(rt);
			REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
			for (Tick t = 1; t <= 400; ++t) {
				step(rt);
			}
			std::vector<Tick> ticks;
			for (const uint32_t sink : sinks) {
				const std::vector<Tick> got = ticks_into(rt, sink, port::kIn);
				REQUIRE(got.size() == 1);
				CHECK(got[0] >= 1 + 60);
				CHECK(got[0] <= 1 + 300);
				ticks.push_back(got[0]);
			}
			return ticks;
		};
		CHECK(fire_ticks(7) == fire_ticks(7));
		CHECK(fire_ticks(7) != fire_ticks(8));
	}

	TEST_CASE("logic: ghost encode / decode / delta, hostile bytes, the 4,096 cap") {
		G g;
		std::vector<uint32_t> bools;
		for (int k = 0; k < 11; ++k) {
			const uint32_t b = g.node(LogicTypeId::SyncedBool);
			g.b.add_param(b, port::kIn, v_bool(k % 3 == 0));
			bools.push_back(b);
		}
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		g.b.add_event(s, port::kSplitterOut, bools[1], port::kSetTrue);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		const LogicGhost base = rt.ghost();
		REQUIRE(rt.ghost_layout().bools.size() == 11);
		const std::vector<uint8_t> bytes = base.encode();
		CHECK(bytes.size() == 10 + 2);
		Result<LogicGhost> back = LogicGhost::decode(bytes, rt.ghost_layout());
		REQUIRE(back.ok());
		CHECK(*back == base);
		REQUIRE(rt.force_event(s, port::kSplitterOut).ok());
		step(rt);
		const LogicGhost next = rt.ghost();
		const std::vector<LogicGhost::BoolDelta> delta = next.bool_delta(base);
		REQUIRE(delta.size() == 1);
		CHECK(delta[0].value);
		CHECK(rt.ghost_layout().bools[delta[0].index] == bools[1]);
		// Hostile bytes: short, long, counts that differ from the layout, padding bits set.
		CHECK_FALSE(LogicGhost::decode(std::span<const uint8_t>(bytes.data(), 5), rt.ghost_layout()).ok());
		std::vector<uint8_t> longer = bytes;
		longer.push_back(0);
		CHECK_FALSE(LogicGhost::decode(longer, rt.ghost_layout()).ok());
		std::vector<uint8_t> counts = bytes;
		counts[4] = 12;
		CHECK_FALSE(LogicGhost::decode(counts, rt.ghost_layout()).ok());
		std::vector<uint8_t> padding = bytes;
		padding[11] |= 0x80;
		CHECK_FALSE(LogicGhost::decode(padding, rt.ghost_layout()).ok());
		// The cap: 4,097 SyncedBools fail create().
		G big;
		for (size_t k = 0; k <= kMaxGhostEntries; ++k) {
			big.node(LogicTypeId::SyncedBool);
		}
		CHECK_FALSE(LogicRuntime::create(big.finish(), {}).ok());
	}

	TEST_CASE("logic: trace hash fixed vector (pins the 40-byte record encoding)") {
		G g;
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		const uint32_t d = g.node(LogicTypeId::Delay);
		g.b.add_param(d, port::kDelay, v_float(0.1));
		const uint32_t b = g.node(LogicTypeId::Bool);
		const uint32_t n = g.node(LogicTypeId::Not);
		g.b.add_event(s, port::kSplitterOut, d, port::kIn);
		g.b.add_event(d, port::kOut, b, port::kSetTrue);
		g.b.add_property(b, port::kValue, n, port::kIn);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		REQUIRE(rt.force_event(s, port::kSplitterOut, LogicContext{ 2, 1 }).ok());
		for (Tick t = 1; t <= 10; ++t) {
			step(rt);
		}
		MESSAGE("trace hash " << rt.trace().hash << " records " << rt.trace().records_total);
		CHECK(rt.trace().records_total == 6);
		CHECK(rt.trace().hash == 0x0449f6ed4956df24ull); // pinned; recompute only when the encoding changes on purpose
	}

	TEST_CASE("logic: Builder rejects bad indices") {
		LogicGraph::Builder b;
		const uint32_t root = b.add_scope(kNoLogicIndex, Guid{}, "root");
		const uint32_t n = b.add_node(LogicTypeId::EventGate, LogicRealm::Server, root, guid_n(1));
		b.add_event(n, port::kOut, 7, port::kIn);
		CHECK_FALSE(b.finish().ok());
		LogicGraph::Builder c;
		c.add_node(LogicTypeId::EventGate, LogicRealm::Server, 5, guid_n(1));
		CHECK_FALSE(c.finish().ok());
	}

	// --- synthetic EBX through build_logic_graph -------------------------------------------------------------------

	TEST_CASE("logic: interface flattening - in, out, pass-through, nested two levels") {
		LogicSyn syn;
		const uint32_t p = syn.add_prefab(); // prefab P with interface D
		const uint32_t q = syn.add_prefab(); // prefab Q nested inside P
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::Server);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t sink2 = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t r = syn.reference(0, static_cast<int>(p));
		const uint32_t d = syn.descriptor(p);
		const uint32_t inner = syn.entity(p, "EventGateEntityData", LogicRealm::Server);
		const uint32_t rq = syn.reference(p, static_cast<int>(q));
		const uint32_t dq = syn.descriptor(q);
		const uint32_t deepest = syn.entity(q, "EventGateEntityData", LogicRealm::Server);
		// Root: A -> P.X, P.Y -> sink, A -> P.X2, P.Y2 -> sink2.
		syn.event(0, a, port::kSplitterOut, r, kX);
		syn.event(0, r, kY, sink, port::kIn);
		syn.event(0, a, port::kSplitterOut, r, kX2);
		syn.event(0, r, kY2, sink2, port::kIn);
		// P: X -> inner.In, inner.Out -> Y; X2 -> Q.X; Q.Y -> Y2.
		syn.event(p, d, kX, inner, port::kIn);
		syn.event(p, inner, port::kOut, d, kY);
		syn.event(p, d, kX2, rq, kX);
		syn.event(p, rq, kY, d, kY2);
		// Q: pass-through X -> Y, and X -> deepest.In.
		syn.event(q, dq, kX, dq, kY);
		syn.event(q, dq, kX, deepest, port::kIn);
		EbxDb db;
		Result<LogicGraph> g = syn.build(db);
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		const uint32_t na = node_of(*g, syn.s, a), nsink = node_of(*g, syn.s, sink), nsink2 = node_of(*g, syn.s, sink2);
		const uint32_t ninner = node_of(*g, syn.s, inner), ndeep = node_of(*g, syn.s, deepest);
		REQUIRE(na != kNoLogicIndex);
		REQUIRE(ninner != kNoLogicIndex);
		REQUIRE(ndeep != kNoLogicIndex);
		CHECK(has_event(*g, na, port::kSplitterOut, ninner, port::kIn)); // in
		CHECK(has_event(*g, ninner, port::kOut, nsink, port::kIn)); // out
		CHECK(has_event(*g, na, port::kSplitterOut, nsink2, port::kIn)); // pass-through two levels deep
		CHECK(has_event(*g, na, port::kSplitterOut, ndeep, port::kIn)); // nested in
		CHECK(g->event_edges().size() == 4);
		CHECK(g->stats().duplicate_paths == 0);
		CHECK(g->stats().dropped_unbound == 0);
		CHECK(g->scopes().size() == 3);
		// find_nodes by nested scope reference: Q's gate is found by P's reference and by Q's.
		CHECK(g->find_nodes(LogicNodeRef{ syn.s.guid(r), Guid{} }, LogicTypeId::EventGate).size() == 2);
		CHECK(g->find_nodes(LogicNodeRef{ syn.s.guid(rq), Guid{} }, LogicTypeId::EventGate) == std::vector<uint32_t>{ ndeep });
		CHECK(g->find_nodes(LogicNodeRef{ syn.s.guid(r), syn.s.guid(deepest) }) == std::vector<uint32_t>{ ndeep });
		CHECK(g->find_nodes(LogicNodeRef{ Guid{}, syn.s.guid(sink) }) == std::vector<uint32_t>{ nsink });
	}

	TEST_CASE("logic: unbound reference, Client realm, TargetType filter, connected vs unconnected unknown types") {
		LogicSyn syn;
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::ClientAndServer);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t client = syn.entity(0, "EventGateEntityData", LogicRealm::Client);
		const uint32_t both = syn.entity(0, "EventGateEntityData", LogicRealm::ClientAndServer);
		const uint32_t unbound = syn.reference(0, -1);
		const uint32_t mystery = syn.entity(0, "MysteryEntityData", LogicRealm::Server);
		syn.entity(0, "MysteryEntityData", LogicRealm::Server); // unconnected
		syn.event(0, a, port::kSplitterOut, unbound, kX); // dropped: unbound
		syn.event(0, a, port::kSplitterOut, client, port::kIn); // dropped on the server: the target is client-only
		syn.event(0, a, port::kSplitterOut, sink, port::kIn, LogicEventTarget::Client); // dropped: TargetType
		syn.event(0, a, port::kSplitterOut, both, port::kIn, LogicEventTarget::NetworkedClient); // networked only
		syn.event(0, a, port::kSplitterOut, both, port::kOpen, LogicEventTarget::NetworkedClientAndServer); // delivered + networked
		syn.event(0, a, port::kSplitterOut, mystery, kX); // into an unknown type: kept, counted
		EbxDb db;
		Result<LogicGraph> g = syn.build(db);
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		const LogicGraphStats &st = g->stats();
		CHECK(st.dropped_unbound == 1);
		CHECK(st.dropped_unbound_events == 1);
		CHECK(st.dropped_realm == 1);
		CHECK(st.dropped_target_type == 1);
		CHECK(st.networked_only_edges == 1);
		CHECK(st.unknown_nodes == 1);
		CHECK(st.unknown_unconnected == 1);
		CHECK(node_of(*g, syn.s, client) == kNoLogicIndex);
		CHECK(g->event_edges().size() == 2);
		REQUIRE(g->networked_edges().size() == 1);
		bool delivered_networked = false;
		uint64_t server_id = 0;
		for (const LogicEdge &e : g->event_edges()) {
			if (e.networked) {
				delivered_networked = true;
				server_id = e.connection;
			}
		}
		CHECK(delivered_networked);
		// The client build of the same data: the client-only gate exists, and the networked edges carry the same ids.
		EbxDb client_db;
		Result<LogicGraph> c = syn.build(client_db, LogicSide::Client); // the same partition content
		REQUIRE(c.ok());
		CHECK(node_of(*c, syn.s, client) != kNoLogicIndex);
		std::vector<uint64_t> client_ids;
		for (const LogicEdge &e : c->networked_edges()) {
			client_ids.push_back(e.connection);
		}
		CHECK(std::find(client_ids.begin(), client_ids.end(), server_id) != client_ids.end());
		CHECK(std::find(client_ids.begin(), client_ids.end(), g->networked_edges()[0].connection) != client_ids.end());
	}

	TEST_CASE("logic: a diamond through two interfaces gives 2 edges and duplicate_paths 1") {
		LogicSyn syn;
		const uint32_t p = syn.add_prefab();
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::Server);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t r1 = syn.reference(0, static_cast<int>(p));
		const uint32_t r2 = syn.reference(0, static_cast<int>(p));
		const uint32_t d = syn.descriptor(p);
		syn.event(p, d, kX, d, kY); // pass-through
		syn.event(0, a, port::kSplitterOut, r1, kX);
		syn.event(0, a, port::kSplitterOut, r2, kX);
		syn.event(0, r1, kY, sink, port::kIn);
		syn.event(0, r2, kY, sink, port::kIn);
		EbxDb db;
		Result<LogicGraph> g = syn.build(db);
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		const uint32_t na = node_of(*g, syn.s, a), nsink = node_of(*g, syn.s, sink);
		REQUIRE(g->event_edges().size() == 2);
		for (const LogicEdge &e : g->event_edges()) {
			CHECK(e.source == na);
			CHECK(e.target == nsink);
		}
		CHECK(g->stats().duplicate_paths == 1);
		CHECK(g->event_edges()[0].connection != g->event_edges()[1].connection); // the last hops differ
	}

	TEST_CASE("logic: an interface cycle terminates (on-path marks)") {
		LogicSyn syn;
		const uint32_t p = syn.add_prefab();
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::Server);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t r = syn.reference(0, static_cast<int>(p));
		const uint32_t d = syn.descriptor(p);
		syn.event(p, d, kX, d, kY); // P: X -> Y
		syn.event(0, a, port::kSplitterOut, r, kX);
		syn.event(0, r, kY, r, kX); // root: P.Y -> P.X, a cycle through the interface
		syn.event(0, r, kY, sink, port::kIn);
		EbxDb db;
		Result<LogicGraph> g = syn.build(db);
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		REQUIRE(g->event_edges().size() == 1);
		CHECK(g->event_edges()[0].source == node_of(*g, syn.s, a));
		CHECK(g->event_edges()[0].target == node_of(*g, syn.s, sink));
		CHECK(g->stats().work < 10000);
	}
}

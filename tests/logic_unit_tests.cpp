#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim_harness/logic_harness.h"
#include "logic_test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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

constexpr const char *kRealms[] = { "Realm_Client", "Realm_Server", "Realm_ClientAndServer", "Realm_None", "Realm_Pipeline" };
constexpr const char *kTargets[] = { "EventConnectionTargetType_Invalid", "EventConnectionTargetType_ClientAndServer",
	"EventConnectionTargetType_Client", "EventConnectionTargetType_Server", "EventConnectionTargetType_NetworkedClient",
	"EventConnectionTargetType_NetworkedClientAndServer" };

uint32_t realm_word(LogicRealm p_realm) {
	return static_cast<uint32_t>(p_realm) + 1;
}

uint32_t target_word(LogicEventTarget p_target) {
	return static_cast<uint32_t>(p_target) + 1;
}

struct LogicSyn {
	SynPartition s;
	struct Prefab {
		uint32_t instance = 0;
		std::vector<uint32_t> objects;
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
		add_prefab();
	}

	uint32_t add_prefab() {
		Prefab p;
		p.instance = s.add({ "PrefabBlueprint", {} });
		prefabs.push_back(p);
		return static_cast<uint32_t>(prefabs.size() - 1);
	}

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

}

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
		CHECK(order == std::vector<uint32_t>{ b1, b2, c1, c2 });
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
		CHECK_FALSE(rt.step().ok());
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
		CHECK(ticks_into(rt, dt_sink, port::kIn) == std::vector<Tick>{ 3 });
		CHECK(ticks_into(rt, mt_sink, port::kIn) == std::vector<Tick>{ 3 + 30 });
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
		CHECK(st.deliveries == 1);
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
		CHECK(rt.trace().hash == 0x0449f6ed4956df24ull);
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

	TEST_CASE("logic: interface flattening - in, out, pass-through, nested two levels") {
		LogicSyn syn;
		const uint32_t p = syn.add_prefab();
		const uint32_t q = syn.add_prefab();
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::Server);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t sink2 = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t r = syn.reference(0, static_cast<int>(p));
		const uint32_t d = syn.descriptor(p);
		const uint32_t inner = syn.entity(p, "EventGateEntityData", LogicRealm::Server);
		const uint32_t rq = syn.reference(p, static_cast<int>(q));
		const uint32_t dq = syn.descriptor(q);
		const uint32_t deepest = syn.entity(q, "EventGateEntityData", LogicRealm::Server);

		syn.event(0, a, port::kSplitterOut, r, kX);
		syn.event(0, r, kY, sink, port::kIn);
		syn.event(0, a, port::kSplitterOut, r, kX2);
		syn.event(0, r, kY2, sink2, port::kIn);

		syn.event(p, d, kX, inner, port::kIn);
		syn.event(p, inner, port::kOut, d, kY);
		syn.event(p, d, kX2, rq, kX);
		syn.event(p, rq, kY, d, kY2);

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
		CHECK(has_event(*g, na, port::kSplitterOut, ninner, port::kIn));
		CHECK(has_event(*g, ninner, port::kOut, nsink, port::kIn));
		CHECK(has_event(*g, na, port::kSplitterOut, nsink2, port::kIn));
		CHECK(has_event(*g, na, port::kSplitterOut, ndeep, port::kIn));
		CHECK(g->event_edges().size() == 4);
		CHECK(g->stats().duplicate_paths == 0);
		CHECK(g->stats().dropped_unbound == 0);
		CHECK(g->scopes().size() == 3);

		CHECK(g->find_nodes(LogicNodeRef{ syn.s.guid(r), Guid{} }, LogicTypeId::EventGate).size() == 2);
		CHECK(g->find_nodes(LogicNodeRef{ syn.s.guid(rq), Guid{} }, LogicTypeId::EventGate) == std::vector<uint32_t>{ ndeep });
		CHECK(g->find_nodes(LogicNodeRef{ syn.s.guid(r), syn.s.guid(deepest) }) == std::vector<uint32_t>{ ndeep });
		CHECK(g->find_nodes(LogicNodeRef{ Guid{}, syn.s.guid(sink) }) == std::vector<uint32_t>{ nsink });

		const auto targets = [&](uint32_t p_scope, int32_t p_port) {
			std::vector<std::pair<uint32_t, int32_t>> out;
			for (const LogicEntry &e : g->entries_of(p_scope, p_port)) {
				out.emplace_back(e.target, e.target_port);
			}
			std::sort(out.begin(), out.end());
			return out;
		};
		const uint32_t scope_p = g->nodes()[ninner].scope;
		const uint32_t scope_q = g->nodes()[ndeep].scope;
		CHECK(targets(scope_p, kX) == std::vector<std::pair<uint32_t, int32_t>>{ { ninner, port::kIn } });
		std::vector<std::pair<uint32_t, int32_t>> deep{ { ndeep, port::kIn }, { nsink2, port::kIn } };
		std::sort(deep.begin(), deep.end());
		CHECK(targets(scope_p, kX2) == deep);
		CHECK(targets(scope_q, kX) == deep);
		CHECK(targets(scope_p, kY).empty());
		CHECK(g->scope_named(g->text(g->scopes()[0].name)) == std::optional<uint32_t>(0));
		CHECK_FALSE(g->scope_named("no such scope").has_value());
	}

	TEST_CASE("logic: unbound reference, Client realm, TargetType filter, connected vs unconnected unknown types") {
		LogicSyn syn;
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::ClientAndServer);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t client = syn.entity(0, "EventGateEntityData", LogicRealm::Client);
		const uint32_t both = syn.entity(0, "EventGateEntityData", LogicRealm::ClientAndServer);
		const uint32_t unbound = syn.reference(0, -1);
		const uint32_t mystery = syn.entity(0, "MysteryEntityData", LogicRealm::Server);
		syn.entity(0, "MysteryEntityData", LogicRealm::Server);
		syn.event(0, a, port::kSplitterOut, unbound, kX);
		syn.event(0, a, port::kSplitterOut, client, port::kIn);
		syn.event(0, a, port::kSplitterOut, sink, port::kIn, LogicEventTarget::Client);
		syn.event(0, a, port::kSplitterOut, both, port::kIn, LogicEventTarget::NetworkedClient);
		syn.event(0, a, port::kSplitterOut, both, port::kOpen, LogicEventTarget::NetworkedClientAndServer);
		syn.event(0, a, port::kSplitterOut, mystery, kX);
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

		EbxDb client_db;
		Result<LogicGraph> c = syn.build(client_db, LogicSide::Client);
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
		syn.event(p, d, kX, d, kY);
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
		CHECK(g->event_edges()[0].connection != g->event_edges()[1].connection);
	}

	TEST_CASE("logic: an interface cycle terminates (on-path marks)") {
		LogicSyn syn;
		const uint32_t p = syn.add_prefab();
		const uint32_t a = syn.entity(0, "EventSplitterEntityData", LogicRealm::Server);
		const uint32_t sink = syn.entity(0, "EventGateEntityData", LogicRealm::Server);
		const uint32_t r = syn.reference(0, static_cast<int>(p));
		const uint32_t d = syn.descriptor(p);
		syn.event(p, d, kX, d, kY);
		syn.event(0, a, port::kSplitterOut, r, kX);
		syn.event(0, r, kY, r, kX);
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

namespace {

LogicValue v_team(int64_t p_team) {
	LogicValue v;
	v.kind = K::Team;
	v.i = p_team;
	return v;
}

LogicValue v_text(G &r_g, std::string_view p_text) {
	LogicValue v;
	v.kind = K::Text;
	v.i = r_g.b.add_text(p_text);
	return v;
}

LogicValue v_transform(float p_x, float p_y, float p_z) {
	LogicValue v;
	v.kind = K::Transform;
	v.m = { 1, 0, 0, 0, 1, 0, 0, 0, 1, p_x, p_y, p_z };
	return v;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

uint32_t event_sink(G &r_g, uint32_t p_source, int32_t p_port) {
	const uint32_t sink = r_g.node(LogicTypeId::EventGate);
	r_g.b.add_event(p_source, p_port, sink, port::kIn);
	return sink;
}

uint32_t property_sink(G &r_g, uint32_t p_source, int32_t p_port) {
	const uint32_t sink = r_g.node(LogicTypeId::PrintDebugText);
	r_g.b.add_property(p_source, p_port, sink, logic_port_id("Text"));
	return sink;
}

uint32_t driver(G &r_g, uint32_t p_target, int32_t p_port) {
	const uint32_t s = r_g.node(LogicTypeId::EventSplitter);
	r_g.b.add_event(s, port::kSplitterOut, p_target, p_port);
	return s;
}

struct Counter {
	uint32_t node = 0, inc = 0, dec = 0;
};
Counter counter(G &r_g, int64_t p_default = 0) {
	Counter c;
	c.node = r_g.node(LogicTypeId::Int);
	r_g.b.add_param(c.node, logic_port_id("DefaultValue"), v_int(p_default));
	r_g.b.add_param(c.node, logic_port_id("IncDecValue"), v_int(1));
	c.inc = driver(r_g, c.node, logic_port_id("Increment"));
	c.dec = driver(r_g, c.node, logic_port_id("Decrement"));
	return c;
}

void force(LogicRuntime &r_rt, uint32_t p_driver, LogicContext p_context = {}) {
	REQUIRE(r_rt.force_event(p_driver, port::kSplitterOut, p_context).ok());
}

std::vector<Tick> hits(const LogicRuntime &p_rt, uint32_t p_sink) {
	return ticks_into(p_rt, p_sink, port::kIn);
}

std::vector<Tick> property_ticks(const LogicRuntime &p_rt, uint32_t p_target) {
	std::vector<Tick> out;
	for (const LogicTraceRecord &r : p_rt.trace().records) {
		if (r.kind == LogicTraceKind::Property && r.target == p_target) {
			out.push_back(r.tick);
		}
	}
	return out;
}

std::vector<int> order_at(const LogicRuntime &p_rt, Tick p_tick, const std::vector<uint32_t> &p_sinks) {
	std::vector<int> out;
	for (const LogicTraceRecord &r : p_rt.trace().records) {
		if (r.kind != LogicTraceKind::Event || r.tick != p_tick) {
			continue;
		}
		for (size_t k = 0; k < p_sinks.size(); ++k) {
			if (p_sinks[k] == r.target) {
				out.push_back(static_cast<int>(k));
			}
		}
	}
	return out;
}

LogicValue out_value(const LogicRuntime &p_rt, uint32_t p_node, int32_t p_port, K p_kind) {
	const std::optional<LogicValue> v = p_rt.output(p_node, p_port);
	REQUIRE(v.has_value());
	REQUIRE(v->kind == p_kind);
	return *v;
}
bool out_bool(const LogicRuntime &p_rt, uint32_t p_node, int32_t p_port = port::kOut) {
	return out_value(p_rt, p_node, p_port, K::Bool).b;
}
int64_t out_int(const LogicRuntime &p_rt, uint32_t p_node, int32_t p_port = port::kOut) {
	return out_value(p_rt, p_node, p_port, K::Int).i;
}
double out_float(const LogicRuntime &p_rt, uint32_t p_node, int32_t p_port = port::kOut) {
	return out_value(p_rt, p_node, p_port, K::Float).f;
}
std::string out_text(const LogicRuntime &p_rt, uint32_t p_node, int32_t p_port) {
	return std::string(p_rt.graph().text(static_cast<uint32_t>(out_value(p_rt, p_node, p_port, K::Text).i)));
}

void no_unhandled(const LogicRuntime &p_rt) {
	for (const LogicUnhandledPort &u : p_rt.unhandled()) {
		MESSAGE("unhandled: type " << int(u.type) << " port " << u.port << (u.event ? " event" : " property") << " x" << u.count);
	}
	CHECK(p_rt.unhandled().empty());
}

const int32_t kOutPorts[10] = { logic_port_id("Out0"), logic_port_id("Out1"), logic_port_id("Out2"), logic_port_id("Out3"), logic_port_id("Out4"),
	logic_port_id("Out5"), logic_port_id("Out6"), logic_port_id("Out7"), logic_port_id("Out8"), logic_port_id("Out9") };
const int32_t kInPorts[4] = { logic_port_id("In0"), logic_port_id("In1"), logic_port_id("In2"), logic_port_id("In3") };
const int32_t kOutcomes[6] = { logic_port_id("A<B"), logic_port_id("A<=B"), logic_port_id("A=B"), logic_port_id("A!=B"), logic_port_id("A>=B"),
	logic_port_id("A>B") };
enum Outcome { kLt = 0, kLe, kEq, kNe, kGe, kGt };

std::vector<uint32_t> outcome_sinks(G &r_g, uint32_t p_node) {
	std::vector<uint32_t> out;
	for (const int32_t p : kOutcomes) {
		out.push_back(event_sink(r_g, p_node, p));
	}
	return out;
}

}

TEST_SUITE("logic-unit") {
	TEST_CASE("logic: Sequence - Time / Event.Id events, OnFinished, Looping, Start while playing, Stop, Pause, Reset, AutoStart, ghost") {
		G g;
		const int32_t kE0 = logic_port_id("SeqE0"), kE1 = logic_port_id("SeqE1");
		const int32_t kOnStarted = logic_port_id("OnStarted"), kPause = logic_port_id("Pause");
		const auto sequence = [&](LogicTypeId p_type, LogicRealm p_realm, double p_length_ms, std::vector<std::pair<double, int32_t>> p_events,
									  bool p_looping, bool p_auto_start) {
			const uint32_t q = g.node(p_type, p_realm);
			g.b.add_param(q, logic_port_id("SequenceLength"), v_float(p_length_ms));
			for (uint16_t k = 0; k < p_events.size(); ++k) {
				g.b.add_param(q, logic_port_id("Time"), v_float(p_events[k].first), k);
				g.b.add_param(q, logic_port_id("Event.Id"), v_int(p_events[k].second), k);
			}
			if (p_looping) {
				g.b.add_param(q, logic_port_id("Looping"), v_bool(true));
			}
			if (p_auto_start) {
				g.b.add_param(q, logic_port_id("AutoStart"), v_bool(true));
			}
			return q;
		};
		struct Sinks {
			uint32_t e0, e1, started, finished, stopped;
		};
		const auto sinks = [&](uint32_t p_q) {
			return Sinks{ event_sink(g, p_q, kE0), event_sink(g, p_q, kE1), event_sink(g, p_q, kOnStarted), event_sink(g, p_q, port::kOnFinished),
				event_sink(g, p_q, port::kOnStopped) };
		};

		const uint32_t a = sequence(LogicTypeId::Sequence, LogicRealm::Server, 1000, { { 0, kE0 }, { 100, kE1 }, { 500, kE0 } }, false, false);
		const Sinks sa = sinks(a);
		const uint32_t a_start = driver(g, a, port::kStart);

		const uint32_t b = sequence(LogicTypeId::Sequence, LogicRealm::Server, 200, { { 50, kE0 } }, true, false);
		const Sinks sb = sinks(b);
		const uint32_t b_start = driver(g, b, port::kStart);

		const uint32_t c = sequence(LogicTypeId::SyncedSequence, LogicRealm::ClientAndServer, 1000, { { 300, kE0 } }, false, false);
		const Sinks sc = sinks(c);
		const uint32_t c_start = driver(g, c, port::kStart), c_stop = driver(g, c, port::kStop), c_pause = driver(g, c, kPause);

		const uint32_t d = sequence(LogicTypeId::Sequence, LogicRealm::ClientAndServer, 100, { { 0, kE1 } }, false, true);
		const Sinks sd = sinks(d);

		const uint32_t e = sequence(LogicTypeId::Sequence, LogicRealm::Server, 100, { { 200, kE0 } }, false, false);
		const Sinks se = sinks(e);
		const uint32_t e_start = driver(g, e, port::kStart);

		const uint32_t f = sequence(LogicTypeId::Sequence, LogicRealm::Server, 1000, { { 100, kE0 } }, false, false);
		const Sinks sf = sinks(f);
		const uint32_t f_start = driver(g, f, port::kStart), f_reset = driver(g, f, port::kReset);

		LogicRuntime rt = make_runtime(g.finish());

		REQUIRE(rt.ghost_layout().sequences == std::vector<uint32_t>{ c, d });
		start(rt);
		CHECK(rt.ghost().sequence_start == std::vector<Tick>{ LogicGhost::kNoStart, 0 });
		std::vector<std::pair<Tick, Tick>> c_ghost;
		for (Tick t = 1; t <= 80; ++t) {
			if (t == 1) {
				for (const uint32_t s : { a_start, b_start, c_start, e_start, f_start }) {
					force(rt, s);
				}
			}
			if (t == 5) {
				force(rt, c_stop);
				force(rt, f_reset);
			}
			if (t == 10) {
				force(rt, c_start);
			}
			if (t == 30) {
				force(rt, c_pause);
			}
			if (t == 32) {
				force(rt, c_stop);
			}
			if (t == 41) {
				force(rt, b_start);
			}
			step(rt);
			if (t == 4 || t == 5 || t == 20 || t == 30) {
				c_ghost.emplace_back(t, rt.ghost().sequence_start[0]);
			}
			if (t == 6) {
				CHECK(rt.ghost().sequence_start[1] == LogicGhost::kNoStart);
			}
		}
		CHECK(c_ghost == std::vector<std::pair<Tick, Tick>>{ { 4, 1 }, { 5, LogicGhost::kNoStart }, { 20, 10 }, { 30, LogicGhost::kNoStart } });
		CHECK(hits(rt, sa.started) == std::vector<Tick>{ 1 });
		CHECK(hits(rt, sa.e0) == std::vector<Tick>{ 1, 31 });
		CHECK(hits(rt, sa.e1) == std::vector<Tick>{ 7 });
		CHECK(hits(rt, sa.finished) == std::vector<Tick>{ 61 });
		CHECK(hits(rt, sa.stopped).empty());
		CHECK(order_at(rt, 1, { sa.started, sa.e0 }) == std::vector<int>{ 0, 1 });

		CHECK(hits(rt, sb.started) == std::vector<Tick>{ 1, 13, 25, 37, 41, 53, 65, 77 });
		CHECK(hits(rt, sb.e0) == std::vector<Tick>{ 4, 16, 28, 40, 44, 56, 68, 80 });
		CHECK(hits(rt, sb.finished) == std::vector<Tick>{ 13, 25, 37, 53, 65, 77 });
		CHECK(order_at(rt, 13, { sb.finished, sb.started }) == std::vector<int>{ 0, 1 });
		CHECK(hits(rt, sc.started) == std::vector<Tick>{ 1, 10 });
		CHECK(hits(rt, sc.stopped) == std::vector<Tick>{ 5 });
		CHECK(hits(rt, sc.e0) == std::vector<Tick>{ 28 });
		CHECK(hits(rt, sc.finished).empty());
		CHECK(hits(rt, sd.started) == std::vector<Tick>{ 0 });
		CHECK(hits(rt, sd.e1) == std::vector<Tick>{ 0 });
		CHECK(hits(rt, sd.finished) == std::vector<Tick>{ 6 });
		CHECK(hits(rt, se.finished) == std::vector<Tick>{ 7 });
		CHECK(hits(rt, se.e0) == std::vector<Tick>{ 13 });
		CHECK(hits(rt, sf.started) == std::vector<Tick>{ 1 });
		CHECK(hits(rt, sf.e0).empty());
		CHECK(hits(rt, sf.stopped).empty());
		CHECK(hits(rt, sf.finished).empty());
		CHECK(rt.ghost().sequence_start == std::vector<Tick>{ LogicGhost::kNoStart, LogicGhost::kNoStart });
		no_unhandled(rt);
	}

	TEST_CASE("logic: EventSwitch - AutoIncrement, NextOut / PreviousOut wrap, Reset, OutEvents clamped to 1..10") {
		G g;
		const auto make = [&](int64_t p_outs, bool p_auto) {
			const uint32_t s = g.node(LogicTypeId::EventSwitch);
			g.b.add_param(s, logic_port_id("OutEvents"), v_int(p_outs));
			if (p_auto) {
				g.b.add_param(s, logic_port_id("AutoIncrement"), v_bool(true));
			}
			std::vector<uint32_t> sinks;
			for (const int32_t p : kOutPorts) {
				sinks.push_back(event_sink(g, s, p));
			}
			return std::make_pair(s, sinks);
		};
		const auto [a, a_sinks] = make(3, true);
		const auto [b, b_sinks] = make(3, false);
		const auto [c, c_sinks] = make(20, true);
		const auto [d, d_sinks] = make(0, true);
		const uint32_t a_in = driver(g, a, port::kIn), c_in = driver(g, c, port::kIn), d_in = driver(g, d, port::kIn);
		const uint32_t b_in = driver(g, b, port::kIn), b_next = driver(g, b, logic_port_id("NextOut")), b_prev = driver(g, b, logic_port_id("PreviousOut"));
		const uint32_t b_reset = driver(g, b, port::kReset);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (Tick t = 1; t <= 11; ++t) {
			if (t <= 4) {
				force(rt, a_in);
			}
			force(rt, c_in);
			if (t <= 2) {
				force(rt, d_in);
			}

			const uint32_t b_script[] = { b_in, b_next, b_in, b_prev, b_prev, b_in, b_reset, b_in };
			if (t <= 8) {
				force(rt, b_script[t - 1]);
			}
			step(rt);
		}
		CHECK(hits(rt, a_sinks[0]) == std::vector<Tick>{ 1, 4 });
		CHECK(hits(rt, a_sinks[1]) == std::vector<Tick>{ 2 });
		CHECK(hits(rt, a_sinks[2]) == std::vector<Tick>{ 3 });
		CHECK(hits(rt, a_sinks[3]).empty());
		CHECK(hits(rt, b_sinks[0]) == std::vector<Tick>{ 1, 8 });
		CHECK(hits(rt, b_sinks[1]) == std::vector<Tick>{ 3 });
		CHECK(hits(rt, b_sinks[2]) == std::vector<Tick>{ 6 });
		CHECK(hits(rt, c_sinks[0]) == std::vector<Tick>{ 1, 11 });
		for (int k = 1; k < 10; ++k) {
			CAPTURE(k);
			CHECK(hits(rt, c_sinks[static_cast<size_t>(k)]) == std::vector<Tick>{ static_cast<Tick>(k + 1) });
		}
		CHECK(hits(rt, d_sinks[0]) == std::vector<Tick>{ 1, 2 });
		CHECK(hits(rt, d_sinks[1]).empty());
		no_unhandled(rt);
	}

	TEST_CASE("logic: RandomMultiEvent - weights, UniformDistribution, DisableOutputOnTrigger, ResetOutputsWhenAllHasTriggered, seeds") {
		struct Built {
			std::shared_ptr<const LogicGraph> graph;
			std::vector<uint32_t> drivers;
			std::vector<std::vector<uint32_t>> sinks;
		};
		const auto build = [] {
			G g;
			Built out;
			const auto make = [&](std::vector<double> p_weights, bool p_uniform, bool p_disable, bool p_reset) {
				const uint32_t r = g.node(LogicTypeId::RandomMultiEvent);
				for (uint16_t k = 0; k < p_weights.size(); ++k) {
					g.b.add_param(r, logic_port_id("RandomEventWeight"), v_float(p_weights[k]), k);
				}
				g.b.add_param(r, logic_port_id("UniformDistribution"), v_bool(p_uniform));
				g.b.add_param(r, logic_port_id("DisableOutputOnTrigger"), v_bool(p_disable));
				g.b.add_param(r, logic_port_id("ResetOutputsWhenAllHasTriggered"), v_bool(p_reset));
				std::vector<uint32_t> sinks;
				for (const int32_t p : kOutPorts) {
					sinks.push_back(event_sink(g, r, p));
				}
				out.drivers.push_back(driver(g, r, port::kIn));
				out.sinks.push_back(sinks);
			};
			make({ 1, 0, 3 }, false, false, false);
			make({ 0, 0, 0 }, true, true, false);
			make({ 0, 0, 0 }, true, true, true);
			make(std::vector<double>(12, 1.0), false, false, false);
			make({}, false, false, false);
			make({ 0, -1, kNaN }, false, false, false);
			out.graph = g.finish();
			return out;
		};
		const auto run = [&](uint64_t p_seed, Built &r_built) {
			LogicRuntimeOptions o;
			o.root_seed = p_seed;
			LogicRuntime rt = make_runtime(r_built.graph, o);
			start(rt);
			for (Tick t = 1; t <= 6; ++t) {
				if (t == 1) {
					for (int k = 0; k < 400; ++k) {
						force(rt, r_built.drivers[0]);
					}
					for (int k = 0; k < 200; ++k) {
						force(rt, r_built.drivers[3]);
					}
					force(rt, r_built.drivers[4]);
					force(rt, r_built.drivers[5]);
				}
				if (t <= 4) {
					force(rt, r_built.drivers[1]);
				}
				force(rt, r_built.drivers[2]);
				step(rt);
			}
			return rt;
		};
		Built built = build();
		LogicRuntime rt = run(7, built);

		const std::vector<int> picks = order_at(rt, 1, built.sinks[0]);
		REQUIRE(picks.size() == 400);
		const auto count = [&](int p_k) { return std::count(picks.begin(), picks.end(), p_k); };
		CHECK(count(1) == 0);
		CHECK(count(0) + count(2) == 400);
		CHECK(count(2) >= 250);
		CHECK(count(2) <= 350);

		std::vector<int> once;
		for (Tick t = 1; t <= 3; ++t) {
			const std::vector<int> at = order_at(rt, t, built.sinks[1]);
			REQUIRE(at.size() == 1);
			once.push_back(at[0]);
		}
		std::vector<int> sorted = once;
		std::sort(sorted.begin(), sorted.end());
		CHECK(sorted == std::vector<int>{ 0, 1, 2 });
		CHECK(order_at(rt, 4, built.sinks[1]).empty());

		for (Tick round = 0; round < 2; ++round) {
			std::vector<int> perm;
			for (Tick t = 1 + 3 * round; t <= 3 + 3 * round; ++t) {
				const std::vector<int> at = order_at(rt, t, built.sinks[2]);
				REQUIRE(at.size() == 1);
				perm.push_back(at[0]);
			}
			std::sort(perm.begin(), perm.end());
			CHECK(perm == std::vector<int>{ 0, 1, 2 });
		}

		const std::vector<int> ten = order_at(rt, 1, built.sinks[3]);
		CHECK(ten.size() == 200);
		for (int k = 0; k < 10; ++k) {
			CAPTURE(k);
			CHECK(std::count(ten.begin(), ten.end(), k) > 0);
		}
		CHECK(order_at(rt, 1, built.sinks[4]).empty());
		CHECK(order_at(rt, 1, built.sinks[5]).empty());
		no_unhandled(rt);

		Built again = build();
		CHECK(order_at(run(7, again), 1, again.sinks[0]) == picks);
		Built other = build();
		CHECK(order_at(run(8, other), 1, other.sinks[0]) != picks);
	}

	TEST_CASE("logic: TeamFilter and PlayerFilter - match, InvertFilter, OnTriggerOnlyForSpecifiedTeam addressed to the filter's team") {
		G g;
		const uint32_t tf = g.node(LogicTypeId::TeamFilter);
		g.b.add_param(tf, logic_port_id("Team"), v_team(1));
		const uint32_t tfi = g.node(LogicTypeId::TeamFilter);
		g.b.add_param(tfi, logic_port_id("Team"), v_team(1));
		g.b.add_param(tfi, logic_port_id("InvertFilter"), v_bool(true));
		const uint32_t pf = g.node(LogicTypeId::PlayerFilter);
		const uint32_t pfi = g.node(LogicTypeId::PlayerFilter);
		g.b.add_param(pfi, logic_port_id("InvertFilter"), v_bool(true));
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		for (const uint32_t n : { tf, tfi, pf, pfi }) {
			g.b.add_event(s, port::kSplitterOut, n, port::kIn);
		}
		const int32_t kOnlyForPlayer = logic_port_id("OnTriggerOnlyForPlayer");
		const uint32_t tf_match = event_sink(g, tf, port::kOnTriggeredByMatchingTeam), tf_spec = event_sink(g, tf, port::kOnTriggerOnlyForSpecifiedTeam);
		const uint32_t tfi_match = event_sink(g, tfi, port::kOnTriggeredByMatchingTeam), tfi_spec = event_sink(g, tfi, port::kOnTriggerOnlyForSpecifiedTeam);
		const uint32_t pf_out = event_sink(g, pf, kOnlyForPlayer), pfi_out = event_sink(g, pfi, kOnlyForPlayer);
		LogicRuntime rt = make_runtime(g.finish());
		REQUIRE(rt.watch(tf_spec, port::kIn, LogicWatchKind::EventIn).ok());
		REQUIRE(rt.watch(tf_match, port::kIn, LogicWatchKind::EventIn).ok());
		start(rt);
		const LogicContext contexts[] = { { 4, 1 }, { 5, 2 }, { -1, 0 } };
		std::vector<std::tuple<Tick, uint32_t, int32_t, int>> seen;
		for (Tick t = 1; t <= 3; ++t) {
			force(rt, s, contexts[t - 1]);
			step(rt);
			for (const LogicWatchHit &h : rt.watched()) {
				seen.emplace_back(h.tick, h.node, h.context.player, int(h.context.team));
			}
		}
		CHECK(hits(rt, tf_match) == std::vector<Tick>{ 1 });
		CHECK(hits(rt, tf_spec) == std::vector<Tick>{ 1, 2, 3 });
		CHECK(hits(rt, tfi_match) == std::vector<Tick>{ 2, 3 });
		CHECK(hits(rt, tfi_spec) == std::vector<Tick>{ 1, 2, 3 });
		CHECK(hits(rt, pf_out) == std::vector<Tick>{ 1, 2 });
		CHECK(hits(rt, pfi_out) == std::vector<Tick>{ 3 });

		CHECK(seen == std::vector<std::tuple<Tick, uint32_t, int32_t, int>>{ { 1, tf_match, 4, 1 }, { 1, tf_spec, 4, 1 }, { 2, tf_spec, 5, 1 },
								 { 3, tf_spec, -1, 1 } });
		CHECK(order_at(rt, 1, { tf_match, tf_spec }) == std::vector<int>{ 0, 1 });
		no_unhandled(rt);
	}

	TEST_CASE("logic: Not, And, Or, Or4 - truth tables, unconnected inputs, propagation on change only") {
		G g;
		std::vector<std::pair<uint32_t, bool>> expect;
		const auto gate = [&](LogicTypeId p_type, std::vector<bool> p_inputs, bool p_expected) {
			const uint32_t n = g.node(p_type);
			const int32_t ports[] = { port::kIn1, port::kIn2, port::kIn3, port::kIn4 };
			for (size_t k = 0; k < p_inputs.size(); ++k) {
				g.b.add_param(n, p_type == LogicTypeId::Not ? port::kIn : ports[k], v_bool(p_inputs[k]));
			}
			expect.emplace_back(n, p_expected);
		};
		for (const int bits : { 0, 1, 2, 3 }) {
			const bool x = (bits & 1) != 0, y = (bits & 2) != 0;
			gate(LogicTypeId::And, { x, y }, x && y);
			gate(LogicTypeId::Or, { x, y }, x || y);
		}
		for (const int bits : { 0, 1, 2, 4, 8, 15 }) {
			gate(LogicTypeId::Or4, { (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0 }, bits != 0);
		}
		gate(LogicTypeId::Not, { false }, true);
		gate(LogicTypeId::Not, { true }, false);
		gate(LogicTypeId::Not, {}, true);
		gate(LogicTypeId::And, {}, false);
		gate(LogicTypeId::Or4, {}, false);

		const uint32_t b1 = g.node(LogicTypeId::Bool);
		const uint32_t b2 = g.node(LogicTypeId::Bool);
		g.b.add_param(b2, logic_port_id("DefaultValue"), v_bool(true));
		const uint32_t and1 = g.node(LogicTypeId::And), or1 = g.node(LogicTypeId::Or), not1 = g.node(LogicTypeId::Not);
		g.b.add_property(b1, port::kValue, and1, port::kIn1);
		g.b.add_property(b2, port::kValue, and1, port::kIn2);
		g.b.add_property(b1, port::kValue, or1, port::kIn1);
		g.b.add_property(b2, port::kValue, or1, port::kIn2);
		g.b.add_property(b1, port::kValue, not1, port::kIn);
		const uint32_t bte = g.node(LogicTypeId::BoolToEvent);
		g.b.add_property(and1, port::kOut, bte, port::kValue);
		const uint32_t on_true = event_sink(g, bte, port::kOnTrue), on_false = event_sink(g, bte, port::kOnFalse);
		const uint32_t p_or = property_sink(g, or1, port::kOut), p_not = property_sink(g, not1, port::kOut);
		const uint32_t b1_true = driver(g, b1, port::kSetTrue), b2_false = driver(g, b2, port::kSetFalse), b1_toggle = driver(g, b1, logic_port_id("Toggle"));
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (const auto &[n, want] : expect) {
			CAPTURE(n);
			CHECK(out_bool(rt, n) == want);
		}
		for (Tick t = 1; t <= 5; ++t) {
			if (t == 2) {
				force(rt, b1_true);
			}
			if (t == 3) {
				force(rt, b2_false);
			}
			if (t == 4) {
				force(rt, b1_toggle);
			}
			step(rt);
		}
		CHECK(hits(rt, on_true) == std::vector<Tick>{ 2 });
		CHECK(hits(rt, on_false) == std::vector<Tick>{ 3 });

		CHECK(property_ticks(rt, p_or) == std::vector<Tick>{ 0, 0, 4 });
		CHECK(property_ticks(rt, p_not) == std::vector<Tick>{ 0, 2, 4 });
		CHECK_FALSE(out_bool(rt, or1));
		CHECK(out_bool(rt, not1));
		no_unhandled(rt);
	}

	TEST_CASE("logic: Compare - the six CompareOp operators, floats, NaN, unknown operator, a driven chain") {
		G g;
		const char *ops[] = { "CompareOp_Equal", "CompareOp_NotEqual", "CompareOp_Less", "CompareOp_LessOrEqual", "CompareOp_Greater",
			"CompareOp_GreaterOrEqual" };

		const bool table[6][3] = { { false, true, false }, { true, false, true }, { true, false, false }, { true, true, false }, { false, false, true },
			{ false, true, true } };
		std::vector<std::pair<uint32_t, bool>> expect;
		for (int op = 0; op < 6; ++op) {
			for (int col = 0; col < 3; ++col) {
				const uint32_t n = g.node(LogicTypeId::Compare);
				g.b.add_param(n, logic_port_id("IntIn0"), v_int(col + 1));
				g.b.add_param(n, logic_port_id("IntIn1"), v_int(2));
				g.b.add_param(n, logic_port_id("Operator"), v_text(g, ops[op]));
				expect.emplace_back(n, table[op][col]);
			}

			const uint32_t f = g.node(LogicTypeId::Compare);
			g.b.add_input(f, logic_port_id("FloatIn0"), v_float(1.5));
			g.b.add_input(f, logic_port_id("FloatIn1"), v_float(2.5));
			g.b.add_param(f, logic_port_id("Operator"), v_text(g, ops[op]));
			expect.emplace_back(f, table[op][0]);
			const uint32_t nan = g.node(LogicTypeId::Compare);
			g.b.add_input(nan, logic_port_id("FloatIn0"), v_float(kNaN));
			g.b.add_input(nan, logic_port_id("FloatIn1"), v_float(1.0));
			g.b.add_param(nan, logic_port_id("Operator"), v_text(g, ops[op]));
			expect.emplace_back(nan, false);
		}
		const uint32_t bogus = g.node(LogicTypeId::Compare);
		g.b.add_param(bogus, logic_port_id("Operator"), v_text(g, "CompareOp_Bogus"));
		expect.emplace_back(bogus, false);
		const uint32_t none = g.node(LogicTypeId::Compare);
		expect.emplace_back(none, false);

		const Counter cnt = counter(g);
		const uint32_t ge = g.node(LogicTypeId::Compare);
		g.b.add_param(ge, logic_port_id("IntIn1"), v_int(2));
		g.b.add_param(ge, logic_port_id("Operator"), v_text(g, "CompareOp_GreaterOrEqual"));
		g.b.add_property(cnt.node, port::kValue, ge, logic_port_id("IntIn0"));
		const uint32_t bte = g.node(LogicTypeId::BoolToEvent);
		g.b.add_property(ge, port::kOut, bte, port::kValue);
		const uint32_t on_true = event_sink(g, bte, port::kOnTrue), on_false = event_sink(g, bte, port::kOnFalse);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (const auto &[n, want] : expect) {
			CAPTURE(n);
			CHECK(out_bool(rt, n) == want);
		}
		for (Tick t = 1; t <= 5; ++t) {
			force(rt, t <= 3 ? cnt.inc : cnt.dec);
			step(rt);
		}
		CHECK(hits(rt, on_true) == std::vector<Tick>{ 2 });
		CHECK(hits(rt, on_false) == std::vector<Tick>{ 5 });
		no_unhandled(rt);
	}

	TEST_CASE("logic: CompareInt, CompareFloat, CompareBool - outcome events in fixed order, NaN, TriggerOnPropertyChange, TriggerOnStart") {
		G g;
		const auto cmp = [&](LogicTypeId p_type, LogicValue p_a, LogicValue p_b) {
			const uint32_t n = g.node(p_type);
			g.b.add_param(n, logic_port_id("A"), p_a);
			g.b.add_param(n, logic_port_id("B"), p_b);
			return n;
		};
		const uint32_t s = g.node(LogicTypeId::EventSplitter);
		std::vector<std::vector<uint32_t>> at1;
		std::vector<std::vector<int>> want1;
		const auto triggered = [&](uint32_t p_n, std::vector<int> p_want) {
			g.b.add_event(s, port::kSplitterOut, p_n, port::kIn);
			at1.push_back(outcome_sinks(g, p_n));
			want1.push_back(std::move(p_want));
		};
		triggered(cmp(LogicTypeId::CompareInt, v_int(1), v_int(2)), { kLt, kLe, kNe });
		triggered(cmp(LogicTypeId::CompareInt, v_int(2), v_int(2)), { kLe, kEq, kGe });
		triggered(cmp(LogicTypeId::CompareInt, v_int(3), v_int(2)), { kNe, kGe, kGt });
		triggered(cmp(LogicTypeId::CompareInt, v_float(2.7), v_int(2)), { kLe, kEq, kGe });
		triggered(cmp(LogicTypeId::CompareFloat, v_float(1.0), v_float(1.5)), { kLt, kLe, kNe });
		triggered(cmp(LogicTypeId::CompareFloat, v_float(2.5), v_float(2.5)), { kLe, kEq, kGe });
		triggered(cmp(LogicTypeId::CompareFloat, v_float(kNaN), v_float(1.0)), {});

		const Counter cnt = counter(g);
		const uint32_t on_change = cmp(LogicTypeId::CompareInt, v_int(0), v_int(1));
		g.b.add_param(on_change, logic_port_id("TriggerOnPropertyChange"), v_bool(true));
		g.b.add_property(cnt.node, port::kValue, on_change, logic_port_id("A"));
		const std::vector<uint32_t> change_sinks = outcome_sinks(g, on_change);
		const uint32_t quiet = cmp(LogicTypeId::CompareInt, v_int(0), v_int(1));
		g.b.add_property(cnt.node, port::kValue, quiet, logic_port_id("A"));
		const std::vector<uint32_t> quiet_sinks = outcome_sinks(g, quiet);

		const uint32_t on_start = cmp(LogicTypeId::CompareFloat, v_float(0.0), v_float(1.0));
		g.b.add_param(on_start, logic_port_id("TriggerOnStart"), v_bool(true));
		const std::vector<uint32_t> start_sinks = outcome_sinks(g, on_start);

		const uint32_t cb_const = g.node(LogicTypeId::CompareBool);
		g.b.add_param(cb_const, logic_port_id("Bool"), v_bool(true));
		g.b.add_event(s, port::kSplitterOut, cb_const, port::kIn);
		const uint32_t b = g.node(LogicTypeId::Bool);
		const uint32_t b_true = driver(g, b, port::kSetTrue), b_false = driver(g, b, port::kSetFalse);
		const uint32_t cb_change = g.node(LogicTypeId::CompareBool);
		g.b.add_param(cb_change, logic_port_id("TriggerOnPropertyChange"), v_bool(true));
		g.b.add_property(b, port::kValue, cb_change, logic_port_id("Bool"));
		g.b.add_event(s, port::kSplitterOut, cb_change, port::kIn);
		const uint32_t cb_quiet = g.node(LogicTypeId::CompareBool);
		g.b.add_property(b, port::kValue, cb_quiet, logic_port_id("Bool"));
		const uint32_t cbc_true = event_sink(g, cb_const, port::kOnTrue), cbc_false = event_sink(g, cb_const, port::kOnFalse);
		const uint32_t cbx_true = event_sink(g, cb_change, port::kOnTrue), cbx_false = event_sink(g, cb_change, port::kOnFalse);
		const uint32_t cbq_true = event_sink(g, cb_quiet, port::kOnTrue), cbq_false = event_sink(g, cb_quiet, port::kOnFalse);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (Tick t = 1; t <= 3; ++t) {
			if (t == 1) {
				force(rt, s);
			}
			if (t >= 2) {
				force(rt, cnt.inc);
				force(rt, t == 2 ? b_true : b_false);
			}
			step(rt);
		}
		for (size_t k = 0; k < at1.size(); ++k) {
			CAPTURE(k);
			CHECK(order_at(rt, 1, at1[k]) == want1[k]);
			CHECK(order_at(rt, 0, at1[k]).empty());
			CHECK(order_at(rt, 2, at1[k]).empty());
		}
		CHECK(order_at(rt, 0, change_sinks).empty());
		CHECK(order_at(rt, 1, change_sinks).empty());
		CHECK(order_at(rt, 2, change_sinks) == std::vector<int>{ kLe, kEq, kGe });
		CHECK(order_at(rt, 3, change_sinks) == std::vector<int>{ kNe, kGe, kGt });
		for (Tick t = 0; t <= 3; ++t) {
			CHECK(order_at(rt, t, quiet_sinks).empty());
		}
		CHECK(order_at(rt, 0, start_sinks) == std::vector<int>{ kLt, kLe, kNe });
		CHECK(order_at(rt, 1, start_sinks).empty());
		CHECK(hits(rt, cbc_true) == std::vector<Tick>{ 1 });
		CHECK(hits(rt, cbc_false).empty());
		CHECK(hits(rt, cbx_true) == std::vector<Tick>{ 2 });
		CHECK(hits(rt, cbx_false) == std::vector<Tick>{ 1, 3 });
		CHECK(hits(rt, cbq_true).empty());
		CHECK(hits(rt, cbq_false).empty());
		no_unhandled(rt);
	}

	TEST_CASE("logic: MathOp - operator codes 0..5, left to right, divide by zero, wrapping, int vs float, a driven input") {
		G g;
		std::vector<std::pair<uint32_t, LogicValue>> expect;
		const auto math = [&](std::vector<LogicValue> p_in, std::vector<int64_t> p_ops, LogicValue p_want) {
			const uint32_t n = g.node(LogicTypeId::MathOp);
			for (size_t k = 0; k < p_in.size(); ++k) {
				g.b.add_param(n, kInPorts[k], p_in[k]);
			}
			for (uint16_t k = 0; k < p_ops.size(); ++k) {
				g.b.add_param(n, logic_port_id("Operators"), v_int(p_ops[k]), k);
			}
			expect.emplace_back(n, p_want);
		};
		const int64_t results[] = { 10, 4, 21, 2, 3, 7 };
		for (int64_t op = 0; op < 6; ++op) {
			math({ v_int(7), v_int(3) }, { op }, v_int(results[op]));
		}
		math({ v_int(7), v_int(3) }, { 9 }, v_int(10));
		math({ v_int(7), v_int(0) }, { 3 }, v_int(0));
		math({ v_int(-7), v_int(2) }, { 3 }, v_int(-3));
		math({ v_int(kI64Min), v_int(-1) }, { 3 }, v_int(0));
		math({ v_int(kI64Max), v_int(1) }, { 0 }, v_int(kI64Min));
		math({ v_int(2), v_int(3), v_int(4) }, { 0, 2 }, v_int(20));
		math({ v_float(7.0), v_int(2) }, { 3 }, v_float(3.5));
		math({ v_int(7), v_float(0.0) }, { 3 }, v_float(0.0));
		math({ v_float(1.5), v_float(2.5) }, { 5 }, v_float(2.5));
		math({ v_int(5) }, {}, v_int(5));

		const Counter cnt = counter(g);
		const uint32_t m = g.node(LogicTypeId::MathOp);
		g.b.add_param(m, kInPorts[1], v_int(10));
		g.b.add_param(m, logic_port_id("Operators"), v_int(2));
		g.b.add_property(cnt.node, port::kValue, m, kInPorts[0]);
		const uint32_t sink = property_sink(g, m, port::kOut);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (const auto &[n, want] : expect) {
			CAPTURE(n);
			const std::optional<LogicValue> got = rt.output(n, port::kOut);
			REQUIRE(got.has_value());
			CHECK(got->kind == want.kind);
			CHECK(got->same_as(want));
		}
		CHECK(out_int(rt, m) == 0);
		force(rt, cnt.inc);
		step(rt);
		CHECK(out_int(rt, m) == 10);
		step(rt);
		CHECK(property_ticks(rt, sink) == std::vector<Tick>{ 0, 1 });
		no_unhandled(rt);
	}

	TEST_CASE("logic: IntHub - InputSelect picks In0..In9; an out-of-range select publishes nothing") {
		G g;
		const auto hub = [&] {
			const uint32_t h = g.node(LogicTypeId::IntHub);
			g.b.add_param(h, kInPorts[0], v_int(5));
			g.b.add_param(h, kInPorts[1], v_int(9));
			g.b.add_param(h, kInPorts[2], v_int(11));
			return h;
		};
		const uint32_t fixed = hub();
		g.b.add_param(fixed, logic_port_id("InputSelect"), v_int(1));
		const uint32_t out_of_range = hub();
		g.b.add_param(out_of_range, logic_port_id("InputSelect"), v_int(10));
		const uint32_t driven = hub();
		const Counter cnt = counter(g);
		g.b.add_property(cnt.node, port::kValue, driven, logic_port_id("InputSelect"));
		const uint32_t sink = property_sink(g, driven, port::kOut);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK(out_int(rt, fixed) == 9);
		CHECK_FALSE(rt.output(out_of_range, port::kOut).has_value());
		CHECK(out_int(rt, driven) == 5);
		std::vector<int64_t> seen;
		for (Tick t = 1; t <= 5; ++t) {
			force(rt, t <= 2 ? cnt.inc : cnt.dec);
			step(rt);
			seen.push_back(out_int(rt, driven));
		}
		CHECK(seen == std::vector<int64_t>{ 9, 11, 9, 5, 5 });
		CHECK(property_ticks(rt, sink) == std::vector<Tick>{ 0, 1, 2, 3, 4 });
		no_unhandled(rt);
	}

	TEST_CASE("logic: FloatHub picks In0..In9 by InputSelect; PlatformSplitter fires the runtime's platform; "
			  "deliver_event enters a node's input") {
		G g;
		const uint32_t hub = g.node(LogicTypeId::FloatHub);
		g.b.add_param(hub, kInPorts[0], v_float(4.0));
		g.b.add_param(hub, kInPorts[1], v_float(2.0));
		const Counter cnt = counter(g);
		g.b.add_property(cnt.node, port::kValue, hub, logic_port_id("InputSelect"));
		const uint32_t split = g.node(LogicTypeId::PlatformSplitter, LogicRealm::Client);
		const uint32_t win32 = event_sink(g, split, logic_port_id("Platform_Win32"));
		const uint32_t xenon = event_sink(g, split, logic_port_id("Platform_Xenon"));
		const uint32_t ps3 = event_sink(g, split, logic_port_id("Platform_Ps3"));
		const uint32_t gate = g.node(LogicTypeId::EventGate);
		const uint32_t after_gate = event_sink(g, gate, port::kOut);
		const std::shared_ptr<const LogicGraph> graph = g.finish();
		for (const LogicPlatform platform : { LogicPlatform::Win32, LogicPlatform::Xenon, LogicPlatform::Ps3 }) {
			LogicRuntimeOptions o;
			o.platform = platform;
			LogicRuntime rt = make_runtime(graph, o);
			start(rt);
			CHECK(out_float(rt, hub) == 4.0);
			REQUIRE(rt.deliver_event(split, logic_port_id("Impulse")).ok());
			REQUIRE(rt.deliver_event(gate, port::kIn).ok());
			step(rt);
			CHECK(ticks_into(rt, win32, port::kIn) ==
					(platform == LogicPlatform::Win32 ? std::vector<Tick>{ 1 } : std::vector<Tick>{}));
			CHECK(ticks_into(rt, xenon, port::kIn) ==
					(platform == LogicPlatform::Xenon ? std::vector<Tick>{ 1 } : std::vector<Tick>{}));
			CHECK(ticks_into(rt, ps3, port::kIn) ==
					(platform == LogicPlatform::Ps3 ? std::vector<Tick>{ 1 } : std::vector<Tick>{}));
			CHECK(ticks_into(rt, gate, port::kIn) == std::vector<Tick>{ 1 });
			CHECK(ticks_into(rt, after_gate, port::kIn) == std::vector<Tick>{ 1 });
			force(rt, cnt.inc);
			step(rt);
			CHECK(out_float(rt, hub) == 2.0);
			force(rt, cnt.inc);
			step(rt);
			CHECK(out_float(rt, hub) == 0.0);
			CHECK_FALSE(rt.deliver_event(9999, port::kIn).ok());
			no_unhandled(rt);
		}
	}

	TEST_CASE("logic: Int and Float - Increment, Decrement, Reset to DefaultValue, publish on change only, wrapping") {
		G g;
		const auto number = [&](LogicTypeId p_type, LogicValue p_default, std::optional<LogicValue> p_step) {
			const uint32_t n = g.node(p_type);
			g.b.add_param(n, logic_port_id("DefaultValue"), p_default);
			if (p_step) {
				g.b.add_param(n, logic_port_id("IncDecValue"), *p_step);
			}
			return n;
		};
		const uint32_t i = number(LogicTypeId::Int, v_int(5), v_int(2));
		const uint32_t f = number(LogicTypeId::Float, v_float(1.0), v_float(0.25));
		const uint32_t no_step = number(LogicTypeId::Int, v_int(0), std::nullopt);
		const uint32_t wraps = number(LogicTypeId::Int, v_int(kI64Max), v_int(1));
		const int32_t kInc = logic_port_id("Increment"), kDec = logic_port_id("Decrement");

		const int32_t script[] = { kInc, kInc, kDec, port::kReset, port::kReset };
		std::vector<uint32_t> i_drivers, f_drivers;
		for (const int32_t p : script) {
			i_drivers.push_back(driver(g, i, p));
			f_drivers.push_back(driver(g, f, p));
		}
		const uint32_t no_step_inc = driver(g, no_step, kInc), wraps_inc = driver(g, wraps, kInc);
		const uint32_t i_sink = property_sink(g, i, port::kValue), f_sink = property_sink(g, f, port::kValue);
		const uint32_t no_step_sink = property_sink(g, no_step, port::kValue);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK(out_int(rt, i, port::kValue) == 5);
		CHECK(out_float(rt, f, port::kValue) == 1.0);
		std::vector<int64_t> is;
		std::vector<double> fs;
		for (Tick t = 1; t <= 5; ++t) {
			force(rt, i_drivers[t - 1]);
			force(rt, f_drivers[t - 1]);
			if (t == 1) {
				force(rt, no_step_inc);
				force(rt, wraps_inc);
			}
			step(rt);
			is.push_back(out_int(rt, i, port::kValue));
			fs.push_back(out_float(rt, f, port::kValue));
		}
		CHECK(is == std::vector<int64_t>{ 7, 9, 7, 5, 5 });
		CHECK(fs == std::vector<double>{ 1.25, 1.5, 1.25, 1.0, 1.0 });
		CHECK(property_ticks(rt, i_sink) == std::vector<Tick>{ 0, 1, 2, 3, 4 });
		CHECK(property_ticks(rt, f_sink) == std::vector<Tick>{ 0, 1, 2, 3, 4 });
		CHECK(property_ticks(rt, no_step_sink) == std::vector<Tick>{ 0 });
		CHECK(out_int(rt, wraps, port::kValue) == kI64Min);
		no_unhandled(rt);
	}

	TEST_CASE("logic: PropertyGate - Default, Open publishes the held input, Close holds the output") {
		G g;
		const uint32_t b = g.node(LogicTypeId::Bool);
		const uint32_t b_true = driver(g, b, port::kSetTrue), b_false = driver(g, b, port::kSetFalse);
		const uint32_t closed = g.node(LogicTypeId::PropertyGate);
		g.b.add_param(closed, logic_port_id("Default"), v_bool(false));
		g.b.add_property(b, port::kValue, closed, logic_port_id("BoolIn"));
		const uint32_t open = driver(g, closed, port::kOpen), close = driver(g, closed, port::kClose);
		const uint32_t bool_sink = property_sink(g, closed, logic_port_id("BoolOut"));
		const uint32_t open_by_default = g.node(LogicTypeId::PropertyGate);
		g.b.add_input(open_by_default, logic_port_id("FloatIn"), v_float(2.5));
		const Counter cnt = counter(g);
		g.b.add_property(cnt.node, port::kValue, open_by_default, logic_port_id("IntIn"));
		const uint32_t float_sink = property_sink(g, open_by_default, logic_port_id("FloatOut"));
		const uint32_t int_sink = property_sink(g, open_by_default, logic_port_id("IntOut"));
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK_FALSE(rt.output(closed, logic_port_id("BoolOut")).has_value());
		CHECK(out_float(rt, open_by_default, logic_port_id("FloatOut")) == 2.5);
		for (Tick t = 1; t <= 5; ++t) {
			const uint32_t script[] = { b_true, open, close, b_false, open };
			force(rt, script[t - 1]);
			if (t == 1) {
				force(rt, cnt.inc);
			}
			step(rt);
			if (t == 4) {
				CHECK(out_bool(rt, closed, logic_port_id("BoolOut")));
			}
		}
		CHECK_FALSE(out_bool(rt, closed, logic_port_id("BoolOut")));
		CHECK(property_ticks(rt, bool_sink) == std::vector<Tick>{ 2, 5 });
		CHECK(property_ticks(rt, float_sink) == std::vector<Tick>{ 0 });
		CHECK(property_ticks(rt, int_sink) == std::vector<Tick>{ 0, 1 });
		CHECK(out_int(rt, open_by_default, logic_port_id("IntOut")) == 1);
		no_unhandled(rt);
	}

	TEST_CASE("logic: PropertyCast - Bool, Int and Float casts of every input kind, NaN and clamping") {
		G g;
		const int32_t kToBool = logic_port_id("CastToBool"), kToInt = logic_port_id("CastToInt"), kToFloat = logic_port_id("CastToFloat");
		struct Row {
			uint32_t node;
			bool b;
			int64_t i;
			double f;
		};
		std::vector<Row> rows;
		const auto cast = [&](const char *p_port, LogicValue p_value, bool p_b, int64_t p_i, double p_f) {
			const uint32_t n = g.node(LogicTypeId::PropertyCast);
			g.b.add_input(n, logic_port_id(p_port), p_value);
			rows.push_back({ n, p_b, p_i, p_f });
		};
		cast("IntValue", v_int(3), true, 3, 3.0);
		cast("FloatValue", v_float(-2.7), true, -2, -2.7);
		cast("FloatValue", v_float(1e30), true, kI64Max, 1e30);
		cast("BoolValue", v_bool(false), false, 0, 0.0);
		cast("BoolValue", v_bool(true), true, 1, 1.0);
		const uint32_t nan = g.node(LogicTypeId::PropertyCast);
		g.b.add_input(nan, logic_port_id("FloatValue"), v_float(kNaN));
		const Counter cnt = counter(g);
		const uint32_t driven = g.node(LogicTypeId::PropertyCast);
		g.b.add_property(cnt.node, port::kValue, driven, logic_port_id("IntValue"));
		const uint32_t to_bool_sink = property_sink(g, driven, kToBool), to_int_sink = property_sink(g, driven, kToInt);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (const Row &r : rows) {
			CAPTURE(r.node);
			CHECK(out_bool(rt, r.node, kToBool) == r.b);
			CHECK(out_int(rt, r.node, kToInt) == r.i);
			CHECK(out_float(rt, r.node, kToFloat) == r.f);
		}
		CHECK(out_bool(rt, nan, kToBool));
		CHECK(out_int(rt, nan, kToInt) == 0);
		CHECK(std::isnan(out_float(rt, nan, kToFloat)));
		CHECK_FALSE(out_bool(rt, driven, kToBool));
		force(rt, cnt.inc);
		step(rt);
		force(rt, cnt.inc);
		step(rt);
		CHECK(out_int(rt, driven, kToInt) == 2);
		CHECK(property_ticks(rt, to_int_sink) == std::vector<Tick>{ 0, 1, 2 });
		CHECK(property_ticks(rt, to_bool_sink) == std::vector<Tick>{ 0, 1 });
		no_unhandled(rt);
	}

	TEST_CASE("logic: SwitchPropertyString - StringProperties, the select ids 263676847 -> 0 and 263676846 -> 1, a driven input wins") {
		G g;
		constexpr int32_t kSelect0 = 263676847, kSelect1 = 263676846;
		const int32_t kStringOut = logic_port_id("StringOut");
		const auto make = [&] {
			const uint32_t s = g.node(LogicTypeId::SwitchPropertyString);
			g.b.add_param(s, logic_port_id("StringProperties"), v_text(g, "alpha"), 0);
			g.b.add_param(s, logic_port_id("StringProperties"), v_text(g, "beta"), 1);
			return s;
		};
		const uint32_t plain = make();
		const uint32_t plain_sink = property_sink(g, plain, kStringOut);
		const uint32_t sel0 = driver(g, plain, kSelect0), sel1 = driver(g, plain, kSelect1);
		const uint32_t driven = make();
		g.b.add_input(driven, logic_port_id("StringIn1"), v_text(g, "gamma"));
		const uint32_t driven_sel1 = driver(g, driven, kSelect1);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK(out_text(rt, plain, kStringOut) == "alpha");
		CHECK(out_text(rt, driven, kStringOut) == "alpha");
		std::vector<std::string> seen;
		for (Tick t = 1; t <= 3; ++t) {
			force(rt, t == 1 ? sel1 : sel0);
			if (t == 1) {
				force(rt, driven_sel1);
			}
			step(rt);
			seen.push_back(out_text(rt, plain, kStringOut));
		}
		CHECK(seen == std::vector<std::string>{ "beta", "alpha", "alpha" });
		CHECK(property_ticks(rt, plain_sink) == std::vector<Tick>{ 0, 1, 2 });
		CHECK(out_text(rt, driven, kStringOut) == "gamma");
		no_unhandled(rt);
	}

	TEST_CASE("logic: Setting - a bool name publishes true, an int name 0, otherwise 0.0, on every bound output; into a native at t + 1") {
		G g;
		constexpr int32_t kBoolSetting = 2014001821;
		const int32_t kOther = logic_port_id("SettingOther");
		const uint32_t allowed = g.node(LogicTypeId::Setting);
		g.b.add_param(allowed, logic_port_id("BoolSettingName"), v_text(g, "Server.VehicleSpawnAllowed"));
		const uint32_t allowed_sink = property_sink(g, allowed, kBoolSetting);
		const uint32_t spawner = g.node(LogicTypeId::VehicleSpawn);
		g.b.add_property(allowed, kOther, spawner, port::kEnabled);
		const uint32_t int_setting = g.node(LogicTypeId::Setting);
		g.b.add_param(int_setting, logic_port_id("IntSettingName"), v_text(g, "Server.SomeCount"));
		property_sink(g, int_setting, kBoolSetting);
		const uint32_t unnamed = g.node(LogicTypeId::Setting);
		property_sink(g, unnamed, kBoolSetting);
		const uint32_t unbound = g.node(LogicTypeId::Setting);
		g.b.add_param(unbound, logic_port_id("BoolSettingName"), v_text(g, "Server.VehicleSpawnAllowed"));
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK(out_bool(rt, allowed, kBoolSetting));
		CHECK(out_bool(rt, allowed, kOther));
		CHECK(out_int(rt, int_setting, kBoolSetting) == 0);
		CHECK(out_float(rt, unnamed, kBoolSetting) == 0.0);
		CHECK_FALSE(rt.output(unbound, kBoolSetting).has_value());
		CHECK(property_ticks(rt, allowed_sink) == std::vector<Tick>{ 0 });
		REQUIRE(rt.effects().size() == 1);
		const LogicEffect &e = rt.effects()[0];
		CHECK(e.node == spawner);
		CHECK(e.port == port::kEnabled);
		CHECK(e.apply_tick == 1);
		CHECK(e.kind == LogicEffectKind::Property);
		CHECK(e.value.kind == K::Bool);
		CHECK(e.value.b);
		no_unhandled(rt);
	}

	TEST_CASE("logic: Locator - the Transform param on every bound output at start; a driven Transform republishes") {
		G g;
		const int32_t kTransform = logic_port_id("Transform"), kLocA = logic_port_id("LocA"), kLocB = logic_port_id("LocB"), kTx = logic_port_id("Tx");
		const uint32_t loc = g.node(LogicTypeId::Locator);
		g.b.add_param(loc, kTransform, v_transform(1, 2, 3));
		const uint32_t sink_a = property_sink(g, loc, kLocA), sink_b = property_sink(g, loc, kLocB);
		const uint32_t driven = g.node(LogicTypeId::Locator);
		g.b.add_param(driven, kTransform, v_transform(1, 2, 3));
		const uint32_t driven_sink = property_sink(g, driven, kLocA);
		const uint32_t source = g.node(LogicTypeId::CapturePoint);
		g.b.add_property(source, kTx, driven, kTransform);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK(out_value(rt, loc, kLocA, K::Transform).same_as(v_transform(1, 2, 3)));
		CHECK(out_value(rt, loc, kLocB, K::Transform).same_as(v_transform(1, 2, 3)));
		for (Tick t = 1; t <= 3; ++t) {
			if (t == 2) {
				REQUIRE(rt.force_property(source, kTx, v_transform(4, 5, 6)).ok());
			}
			step(rt);
		}
		CHECK(out_value(rt, driven, kLocA, K::Transform).same_as(v_transform(4, 5, 6)));
		CHECK(out_value(rt, loc, kLocA, K::Transform).same_as(v_transform(1, 2, 3)));
		CHECK(property_ticks(rt, sink_a) == std::vector<Tick>{ 0 });
		CHECK(property_ticks(rt, sink_b) == std::vector<Tick>{ 0 });
		CHECK(property_ticks(rt, driven_sink) == std::vector<Tick>{ 0, 2 });
		no_unhandled(rt);
	}

	TEST_CASE("logic: PrintDebugText Print is handled; uninterpreted deliveries are counted no-ops in first-seen order") {
		G g;
		const uint32_t print = g.node(LogicTypeId::PrintDebugText);
		const uint32_t print_drv = driver(g, print, logic_port_id("Print"));
		const uint32_t print_bad = driver(g, print, port::kEnable);
		const uint32_t i = g.node(LogicTypeId::Int);
		const uint32_t int_set = driver(g, i, logic_port_id("Set"));
		const uint32_t delay = g.node(LogicTypeId::Delay);
		const uint32_t b = g.node(LogicTypeId::Bool);
		g.b.add_property(b, port::kValue, delay, logic_port_id("Bogus"));
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		for (Tick t = 1; t <= 2; ++t) {
			force(rt, print_drv);
			force(rt, int_set);
			if (t == 2) {
				force(rt, print_bad);
			}
			step(rt);
		}
		CHECK(ticks_into(rt, print, logic_port_id("Print")) == std::vector<Tick>{ 1, 2 });
		CHECK(out_int(rt, i, port::kValue) == 0);
		const std::span<const LogicUnhandledPort> u = rt.unhandled();
		REQUIRE(u.size() == 3);
		CHECK(u[0].type == LogicTypeId::Delay);
		CHECK(u[0].port == logic_port_id("Bogus"));
		CHECK_FALSE(u[0].event);
		CHECK(u[0].count == 1);
		CHECK(u[1].type == LogicTypeId::Int);
		CHECK(u[1].port == logic_port_id("Set"));
		CHECK(u[1].event);
		CHECK(u[1].count == 2);
		CHECK(u[2].type == LogicTypeId::PrintDebugText);
		CHECK(u[2].port == port::kEnable);
		CHECK(u[2].count == 1);
	}

	TEST_CASE("logic: EventSplitter RunOnce, EventGate Open/Close, Delay RunOnce/Restart/Stop, MultipleTrigger counts, DelayTrigger Reset, SyncedBool In, ConsoleCommandTrigger") {
		G g;
		const uint32_t sp = g.node(LogicTypeId::EventSplitter);
		g.b.add_param(sp, logic_port_id("RunOnce"), v_bool(true));
		const uint32_t sp_in = driver(g, sp, port::kSplitterIn), sp_sink = event_sink(g, sp, port::kSplitterOut);
		const uint32_t gate = g.node(LogicTypeId::EventGate);
		g.b.add_param(gate, logic_port_id("Default"), v_bool(false));
		const uint32_t gate_in = driver(g, gate, port::kIn), gate_open = driver(g, gate, port::kOpen), gate_close = driver(g, gate, port::kClose);
		const uint32_t gate_sink = event_sink(g, gate, port::kOut);
		const auto delay = [&](bool p_run_once) {
			const uint32_t d = g.node(LogicTypeId::Delay);
			g.b.add_param(d, port::kDelay, v_float(0.5));
			if (p_run_once) {
				g.b.add_param(d, logic_port_id("RunOnce"), v_bool(true));
			}
			return d;
		};
		const uint32_t once = delay(true), restart = delay(false), stop = delay(false);
		const uint32_t once_in = driver(g, once, port::kIn), once_sink = event_sink(g, once, port::kOut);
		const uint32_t restart_in = driver(g, restart, port::kIn), restart_re = driver(g, restart, logic_port_id("Restart"));
		const uint32_t restart_sink = event_sink(g, restart, port::kOut);
		const uint32_t stop_in = driver(g, stop, port::kIn), stop_stop = driver(g, stop, port::kStop), stop_sink = event_sink(g, stop, port::kOut);
		const uint32_t mt = g.node(LogicTypeId::MultipleTrigger);
		g.b.add_param(mt, logic_port_id("TriggerAmount"), v_int(3));
		const uint32_t mt_trigger = driver(g, mt, port::kTrigger), mt_subtract = driver(g, mt, logic_port_id("SubtractCount"));
		const uint32_t mt_reset_count = driver(g, mt, logic_port_id("ResetCount")), mt_disable = driver(g, mt, port::kDisable);
		const uint32_t mt_enable = driver(g, mt, port::kEnable);
		const uint32_t mt_sink = event_sink(g, mt, port::kOnTrigger), mt_zero = event_sink(g, mt, logic_port_id("OnReachedZero"));
		const uint32_t dt = g.node(LogicTypeId::DelayTrigger);
		g.b.add_param(dt, logic_port_id("RunOnce"), v_bool(true));
		const uint32_t dt_trigger = driver(g, dt, port::kTrigger), dt_reset = driver(g, dt, port::kReset), dt_sink = event_sink(g, dt, port::kOnTrigger);
		const uint32_t b = g.node(LogicTypeId::Bool);
		const uint32_t b_true = driver(g, b, port::kSetTrue);
		const uint32_t sb = g.node(LogicTypeId::SyncedBool);
		g.b.add_property(b, port::kValue, sb, port::kIn);
		const uint32_t sb_true = event_sink(g, sb, port::kOnTrue);
		const uint32_t cct = g.node(LogicTypeId::ConsoleCommandTrigger);
		const uint32_t cct_sink = event_sink(g, cct, port::kOnTrigger);
		LogicRuntime rt = make_runtime(g.finish());
		start(rt);
		CHECK_FALSE(out_bool(rt, sb));
		for (Tick t = 1; t <= 45; ++t) {
			if (t <= 2) {
				force(rt, sp_in);
			}
			const uint32_t gate_script[] = { gate_in, gate_open, gate_in, gate_close, gate_in };
			if (t <= 5) {
				force(rt, gate_script[t - 1]);
			}
			if (t == 1 || t == 5) {
				force(rt, once_in);
			}
			if (t == 1) {
				force(rt, restart_in);
				force(rt, stop_in);
			}
			if (t == 10) {
				force(rt, restart_re);
				force(rt, stop_stop);
			}
			if (t == 12) {
				force(rt, stop_in);
			}

			if (t == 1 || t == 2 || t == 6 || t == 7 || (t >= 9 && t <= 11) || (t >= 13 && t <= 15) || (t >= 17 && t <= 19)) {
				force(rt, mt_trigger);
			}
			if (t >= 3 && t <= 5) {
				force(rt, mt_subtract);
			}
			if (t == 8) {
				force(rt, mt_reset_count);
			}
			if (t == 12) {
				force(rt, mt_disable);
			}
			if (t == 16) {
				force(rt, mt_enable);
			}
			if (t == 1 || t == 2 || t == 4) {
				force(rt, dt_trigger);
			}
			if (t == 3) {
				force(rt, dt_reset);
			}
			if (t == 2) {
				force(rt, b_true);
			}
			step(rt);
		}
		CHECK(hits(rt, sp_sink) == std::vector<Tick>{ 1 });
		CHECK(hits(rt, gate_sink) == std::vector<Tick>{ 3 });
		CHECK(hits(rt, once_sink) == std::vector<Tick>{ 31 });
		CHECK(hits(rt, restart_sink) == std::vector<Tick>{ 40 });
		CHECK(hits(rt, stop_sink) == std::vector<Tick>{ 42 });
		CHECK(hits(rt, mt_sink) == std::vector<Tick>{ 11, 19 });
		CHECK(hits(rt, mt_zero) == std::vector<Tick>{ 4 });
		CHECK(hits(rt, dt_sink) == std::vector<Tick>{ 1, 4 });
		CHECK(hits(rt, sb_true) == std::vector<Tick>{ 2 });
		CHECK(out_bool(rt, sb));
		CHECK(rt.ghost().bool_delta(LogicGhost{ 0, 1, { 0 }, {}, {} }).size() == 1);
		CHECK(hits(rt, cct_sink).empty());
		no_unhandled(rt);
	}

	TEST_CASE("logic: run_logic_script refuses more than kMaxLogicScriptTicks ticks") {
		G g;
		g.node(LogicTypeId::EventGate);
		std::shared_ptr<const LogicGraph> graph = g.finish();
		LogicScriptOptions o;
		o.kind = LogicScriptKind::Idle;
		o.ticks = kMaxLogicScriptTicks + 1;
		CHECK_FALSE(run_logic_script(graph, o).ok());
		o.ticks = 10;
		Result<LogicRunReport> r = run_logic_script(graph, o);
		REQUIRE(r.ok());
		CHECK(r->ticks == 10);
	}
}

#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "logic_test_support.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;
using namespace bf3::sim::logic;
using logic_test::SynArray;
using logic_test::SynPartition;
using logic_test::SynType;
using logic_test::SynValues;

namespace {

namespace port = logic_port;
using K = LogicValue::Kind;

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

constexpr double kFastSeconds = 30.0;

Guid guid_n(uint32_t p_n) {
	Guid g{};
	std::memcpy(g.bytes.data(), &p_n, 4);
	g.bytes[15] = 0x46;
	return g;
}

constexpr LogicTypeId kTypes[] = { LogicTypeId::Delay, LogicTypeId::EventGate, LogicTypeId::Sequence, LogicTypeId::TeamFilter, LogicTypeId::SyncedBool,
	LogicTypeId::EventSplitter, LogicTypeId::Bool, LogicTypeId::MultipleTrigger, LogicTypeId::PlayerFilter, LogicTypeId::Float, LogicTypeId::DelayTrigger,
	LogicTypeId::BoolToEvent, LogicTypeId::Compare, LogicTypeId::And, LogicTypeId::Or, LogicTypeId::RandomDelay, LogicTypeId::CompareInt,
	LogicTypeId::SwitchPropertyString, LogicTypeId::SyncedSequence, LogicTypeId::Not, LogicTypeId::MathOp, LogicTypeId::CompareFloat,
	LogicTypeId::PropertyCast, LogicTypeId::PrintDebugText, LogicTypeId::CompareBool, LogicTypeId::EventSwitch, LogicTypeId::Int,
	LogicTypeId::RandomMultiEvent, LogicTypeId::Or4, LogicTypeId::IntHub, LogicTypeId::PropertyGate, LogicTypeId::Setting, LogicTypeId::Locator,
	LogicTypeId::CharacterSpawn, LogicTypeId::CapturePoint, LogicTypeId::HumanPlayer, LogicTypeId::Unknown };

constexpr const char *kPortNames[] = { "In", "Out", "In1", "In2", "In3", "In4", "Value", "SetTrue", "SetFalse", "Toggle", "OnTrue", "OnFalse", "Open",
	"Close", "Enable", "Disable", "Trigger", "OnTrigger", "Reset", "Start", "Stop", "Pause", "Delay", "Restart", "TriggerAmount", "SubtractCount",
	"ResetCount", "OnReachedZero", "A", "B", "Bool", "Out0", "Out1", "Out2", "NextOut", "PreviousOut", "Increment", "Decrement", "BoolIn", "BoolOut",
	"IntIn", "FloatIn", "Team", "Enabled", "OnTriggerOnlyForPlayer", "OnTriggeredByMatchingTeam", "OnTriggerOnlyForSpecifiedTeam", "OnFinished" };

int32_t random_port(std::mt19937_64 &r_rng) {
	const uint64_t pick = r_rng() % (std::size(kPortNames) + 3);
	if (pick == std::size(kPortNames)) {
		return port::kSplitterIn;
	}
	if (pick == std::size(kPortNames) + 1) {
		return port::kSplitterOut;
	}
	if (pick == std::size(kPortNames) + 2) {
		return static_cast<int32_t>(r_rng());
	}
	return logic_port_id(kPortNames[pick]);
}

LogicValue random_value(std::mt19937_64 &r_rng) {
	LogicValue v;
	switch (r_rng() % 6) {
		case 0:
			v.kind = K::Bool;
			v.b = (r_rng() & 1) != 0;
			break;
		case 1:
			v.kind = K::Int;
			v.i = static_cast<int64_t>(r_rng());
			break;
		case 2: {
			v.kind = K::Float;
			const double specials[] = { 0.0, -1.0, 1e300, -0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 0.1, 1e9 };
			v.f = (r_rng() & 1) ? specials[r_rng() % std::size(specials)] : static_cast<double>(r_rng() % 1000) / 100.0;
			break;
		}
		case 3:
			v.kind = K::Team;
			v.i = static_cast<int64_t>(r_rng() % 300) - 20;
			break;
		case 4:
			v.kind = K::Text;
			v.i = 0;
			break;
		default:
			v.kind = K::Transform;
			for (float &f : v.m) {
				f = static_cast<float>(r_rng() % 100);
			}
			break;
	}
	return v;
}

constexpr const char *kParamNames[] = { "Delay", "AutoStart", "RunOnce", "RemoveDuplicateEvents", "MinDelay", "MaxDelay", "Enabled", "AutoTriggerEvent",
	"TriggerAmount", "Default", "OutEvents", "AutoIncrement", "RandomEventWeight", "UniformDistribution", "DisableOutputOnTrigger",
	"ResetOutputsWhenAllHasTriggered", "SequenceLength", "Looping", "PlayInReverse", "Time", "Event.Id", "Team", "InvertFilter", "DefaultValue",
	"IncDecValue", "InputCount", "InputSelect", "Value", "InitalEvent", "In", "Operators", "A", "B", "TriggerOnStart", "TriggerOnPropertyChange" };

std::shared_ptr<const LogicGraph> random_graph(std::mt19937_64 &r_rng, uint32_t p_nodes, uint32_t p_edges) {
	LogicGraph::Builder b;
	const uint32_t root = b.add_scope(kNoLogicIndex, Guid{}, "root");
	for (uint32_t i = 0; i < p_nodes; ++i) {
		const LogicTypeId type = kTypes[r_rng() % std::size(kTypes)];
		const uint32_t n = b.add_node(type, LogicRealm::Server, root, guid_n(i));
		const uint32_t params = static_cast<uint32_t>(r_rng() % 6);
		for (uint32_t k = 0; k < params; ++k) {
			LogicValue v = random_value(r_rng);
			if (v.kind == K::Text) {
				v.kind = K::Int;
			}

			const char *name = kParamNames[r_rng() % std::size(kParamNames)];
			if (std::string(name) == "Delay" && (r_rng() & 1)) {
				v.kind = K::Float;
				v.f = static_cast<double>(r_rng() % 3) * 0.05;
			}
			b.add_param(n, logic_port_id(name), v, static_cast<uint16_t>(r_rng() % 3));
		}
		if (r_rng() % 4 == 0) {
			LogicValue v = random_value(r_rng);
			if (v.kind == K::Text) {
				v.kind = K::Bool;
			}
			b.add_input(n, random_port(r_rng), v);
		}
	}
	for (uint32_t e = 0; e < p_edges; ++e) {
		const uint32_t s = static_cast<uint32_t>(r_rng() % p_nodes);
		const uint32_t t = (r_rng() % 8 == 0) ? s : static_cast<uint32_t>(r_rng() % p_nodes);
		switch (r_rng() % 3) {
			case 0:
				b.add_property(s, random_port(r_rng), t, random_port(r_rng));
				break;
			default:
				b.add_event(s, random_port(r_rng), t, random_port(r_rng), static_cast<LogicEventTarget>(r_rng() % 6), r_rng());
				break;
		}
	}
	Result<LogicGraph> g = b.finish();
	REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
	return std::make_shared<const LogicGraph>(std::move(*g));
}

void run_bounded(const std::shared_ptr<const LogicGraph> &p_graph, std::mt19937_64 &r_rng, uint32_t p_ticks, const LogicRuntimeOptions &p_options) {
	Result<LogicRuntime> created = LogicRuntime::create(p_graph, p_options);
	REQUIRE(created.ok());
	LogicRuntime rt = std::move(*created);
	Result<LogicNativeStubs> stubs = LogicNativeStubs::create(p_graph);
	REQUIRE(stubs.ok());
	Result<LogicTickStats> t0 = rt.start();
	if (!t0) {
		CHECK(rt.faulted());
		return;
	}
	CHECK(t0->deliveries <= p_options.max_deliveries_per_tick);
	std::vector<LogicEffect> effects(rt.effects().begin(), rt.effects().end());
	const uint32_t n = static_cast<uint32_t>(p_graph->nodes().size());
	for (uint32_t t = 1; t <= p_ticks; ++t) {
		if (!stubs->apply(t, effects, rt).ok()) {
			break;
		}
		if (r_rng() % 4 == 0) {
			const uint32_t node = static_cast<uint32_t>(r_rng() % n);
			if (r_rng() & 1) {
				CHECK(rt.force_event(node, random_port(r_rng), LogicContext{ static_cast<int32_t>(r_rng() % 4) - 1, static_cast<uint8_t>(r_rng() % 3) }).ok());
			} else {
				LogicValue v = random_value(r_rng);
				if (v.kind == K::Text) {
					v.kind = K::Bool;
				}
				CHECK(rt.force_property(node, random_port(r_rng), v).ok());
			}
		}
		Result<LogicTickStats> st = rt.step();
		if (!st) {
			CHECK(rt.faulted());
			CHECK_FALSE(rt.step().ok());
			return;
		}
		CHECK(st->deliveries <= p_options.max_deliveries_per_tick);
		effects.assign(rt.effects().begin(), rt.effects().end());
	}
	CHECK_FALSE(rt.force_event(n + 5, port::kIn).ok());

	const LogicGhost ghost = rt.ghost();
	Result<LogicGhost> back = LogicGhost::decode(ghost.encode(), rt.ghost_layout());
	REQUIRE(back.ok());
	CHECK(*back == ghost);
}

constexpr const char *kTargets[] = { "EventConnectionTargetType_Invalid", "EventConnectionTargetType_ClientAndServer",
	"EventConnectionTargetType_Client", "EventConnectionTargetType_Server", "EventConnectionTargetType_NetworkedClient",
	"EventConnectionTargetType_NetworkedClientAndServer" };

std::vector<SynType> syn_types() {
	SynType target{ "EventConnectionTargetType", {}, false, {}, false, true };
	for (const char *t : kTargets) {
		target.fields.push_back({ t, 'i', {} });
	}
	return {
		target,
		{ "EventSpec", { { "Id", 'i' } }, true },
		{ "EventConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceEvent", 'S', "EventSpec" }, { "TargetEvent", 'S', "EventSpec" },
									 { "TargetType", 'e', "EventConnectionTargetType" } },
				true },
		{ "PropertyConnection", { { "Source", 'r' }, { "Target", 'r' }, { "SourceFieldId", 'i' }, { "TargetFieldId", 'i' } }, true },
		{ "DataField", { { "Id", 'i' }, { "Value", 's' } }, true },
		{ "InterfaceDescriptorData", { { "Fields", 'v', "DataField" } } },
		{ "PrefabBlueprint", { { "Objects", 'a' }, { "EventConnections", 'v', "EventConnection" }, { "PropertyConnections", 'v', "PropertyConnection" },
									 { "Descriptor", 'r' } } },
		{ "ReferenceObjectData", { { "Blueprint", 'r' }, { "Excluded", 'b' } } },
		{ "EventSplitterEntityData", { { "RunOnce", 'b' } } },
		{ "EventGateEntityData", { { "Default", 'b' } } },
		{ "BoolEntityData", { { "DefaultValue", 'b' } } },
	};
}

struct SynPrefab {
	uint32_t instance = 0;
	uint32_t descriptor = 0;
	std::vector<uint32_t> objects;
	std::vector<SynValues> events, properties;
};

SynValues event_of(uint32_t p_source, int32_t p_sp, uint32_t p_target, int32_t p_tp, uint32_t p_tt) {
	return { { "Source", SynPartition::ref(p_source) }, { "Target", SynPartition::ref(p_target) }, { "SourceEvent", static_cast<uint32_t>(p_sp) },
		{ "TargetEvent", static_cast<uint32_t>(p_tp) }, { "TargetType", p_tt } };
}

SynPartition random_partition(std::mt19937_64 &r_rng, uint32_t p_prefabs, uint32_t p_connections) {
	SynPartition s;
	s.types = syn_types();
	std::vector<SynPrefab> prefabs(p_prefabs);
	for (SynPrefab &p : prefabs) {
		p.instance = s.add({ "PrefabBlueprint", {} });
	}
	for (SynPrefab &p : prefabs) {
		p.descriptor = s.add({ "InterfaceDescriptorData", {} });
		const uint32_t entities = 1 + static_cast<uint32_t>(r_rng() % 4);
		for (uint32_t k = 0; k < entities; ++k) {
			const char *types[] = { "EventSplitterEntityData", "EventGateEntityData", "BoolEntityData" };
			p.objects.push_back(s.add({ types[r_rng() % 3], {} }));
		}
		const uint32_t refs = static_cast<uint32_t>(r_rng() % 3);
		for (uint32_t k = 0; k < refs; ++k) {
			const uint32_t child = static_cast<uint32_t>(1 + r_rng() % (p_prefabs - 1));
			p.objects.push_back(s.add({ "ReferenceObjectData", { { "Blueprint", SynPartition::ref(prefabs[child].instance) } } }));
		}
	}
	for (SynPrefab &p : prefabs) {
		std::vector<uint32_t> ends = p.objects;
		ends.push_back(p.descriptor);
		for (uint32_t c = 0; c < p_connections; ++c) {
			const uint32_t a = ends[r_rng() % ends.size()], b = ends[r_rng() % ends.size()];
			const int32_t sp = static_cast<int32_t>(r_rng() % 4), tp = static_cast<int32_t>(r_rng() % 4);
			if (r_rng() % 3 == 0) {
				p.properties.push_back({ { "Source", SynPartition::ref(a) }, { "Target", SynPartition::ref(b) }, { "SourceFieldId", static_cast<uint32_t>(sp) },
						{ "TargetFieldId", static_cast<uint32_t>(tp) } });
			} else {
				p.events.push_back(event_of(a, sp, b, tp, static_cast<uint32_t>(1 + r_rng() % 6)));
			}
		}
	}
	for (SynPrefab &p : prefabs) {
		std::vector<uint32_t> refs;
		for (const uint32_t o : p.objects) {
			refs.push_back(SynPartition::ref(o));
		}
		s.instances[p.instance].values = { { "Objects", s.add_array(SynArray{ {}, refs, {} }) },
			{ "EventConnections", s.add_array(SynArray{ "EventConnection", {}, p.events }) },
			{ "PropertyConnections", s.add_array(SynArray{ "PropertyConnection", {}, p.properties }) },
			{ "Descriptor", SynPartition::ref(p.descriptor) } };
	}
	return s;
}

SynPartition diamond_chain(uint32_t p_k, uint32_t p_refs = 2, uint32_t p_parallel = 1) {
	SynPartition s;
	s.types = syn_types();
	std::vector<SynPrefab> prefabs(p_k + 1);
	for (SynPrefab &p : prefabs) {
		p.instance = s.add({ "PrefabBlueprint", {} });
	}
	const uint32_t source = s.add({ "EventSplitterEntityData", {} });
	const uint32_t sink = s.add({ "EventGateEntityData", {} });
	for (uint32_t i = 0; i <= p_k; ++i) {
		SynPrefab &p = prefabs[i];
		p.descriptor = s.add({ "InterfaceDescriptorData", {} });
		const uint32_t in = i == 0 ? source : p.descriptor;
		const uint32_t out = i == 0 ? sink : p.descriptor;
		const int32_t in_port = i == 0 ? port::kSplitterOut : 1, out_port = i == 0 ? port::kIn : 2;
		if (i == 0) {
			p.objects = { source, sink };
		}
		if (i == p_k) {
			p.events.push_back(event_of(in, in_port, out, out_port, 4));
			continue;
		}
		for (uint32_t c = 0; c < p_refs; ++c) {
			const uint32_t r = s.add({ "ReferenceObjectData", { { "Blueprint", SynPartition::ref(prefabs[i + 1].instance) } } });
			p.objects.push_back(r);
			for (uint32_t k = 0; k < p_parallel; ++k) {
				p.events.push_back(event_of(in, in_port, r, 1, 4));
				p.events.push_back(event_of(r, 2, out, out_port, 4));
			}
		}
	}
	for (SynPrefab &p : prefabs) {
		std::vector<uint32_t> refs;
		for (const uint32_t o : p.objects) {
			refs.push_back(SynPartition::ref(o));
		}
		s.instances[p.instance].values = { { "Objects", s.add_array(SynArray{ {}, refs, {} }) },
			{ "EventConnections", s.add_array(SynArray{ "EventConnection", {}, p.events }) }, { "Descriptor", SynPartition::ref(p.descriptor) } };
	}
	return s;
}

}

TEST_SUITE("logic-fuzz") {
	TEST_CASE("logic fuzz: random Builder graphs run 600 ticks bounded") {
		std::mt19937_64 rng(0x5eed54);
		const auto start = std::chrono::steady_clock::now();
		for (int round = 0; round < 300; ++round) {
			CAPTURE(round);
			const uint32_t nodes = 2 + static_cast<uint32_t>(rng() % 60);
			const uint32_t edges = static_cast<uint32_t>(rng() % 400);
			std::shared_ptr<const LogicGraph> g = random_graph(rng, nodes, edges);
			LogicRuntimeOptions o;
			o.root_seed = rng();
			o.max_queue = 1 + rng() % 20000;
			o.max_timers = 1 + rng() % 5000;
			run_bounded(g, rng, 600, o);
		}
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("logic fuzz: Delay 0 cycles, self-loops and a fan-out of 10^4") {
		const auto start = std::chrono::steady_clock::now();
		LogicGraph::Builder b;
		const uint32_t root = b.add_scope(kNoLogicIndex, Guid{}, "root");
		const uint32_t s = b.add_node(LogicTypeId::EventSplitter, LogicRealm::Server, root, guid_n(1));
		const uint32_t d = b.add_node(LogicTypeId::Delay, LogicRealm::Server, root, guid_n(2));
		const uint32_t gate = b.add_node(LogicTypeId::EventGate, LogicRealm::Server, root, guid_n(3));
		b.add_event(s, port::kSplitterOut, d, port::kIn);
		b.add_event(d, port::kOut, s, port::kSplitterIn);
		b.add_event(gate, port::kOut, gate, port::kIn);
		b.add_event(s, port::kSplitterOut, gate, port::kIn);
		for (int k = 0; k < 10000; ++k) {
			b.add_event(s, port::kSplitterOut, gate, port::kClose);
		}
		Result<LogicGraph> built = b.finish();
		REQUIRE(built.ok());
		std::shared_ptr<const LogicGraph> g = std::make_shared<const LogicGraph>(std::move(*built));
		std::mt19937_64 rng(1);
		LogicRuntimeOptions o;
		Result<LogicRuntime> created = LogicRuntime::create(g, o);
		REQUIRE(created.ok());
		LogicRuntime rt = std::move(*created);
		REQUIRE(rt.start().ok());
		REQUIRE(rt.force_event(s, port::kSplitterOut).ok());

		bool faulted = false;
		size_t carried_ticks = 0;
		for (int t = 1; t <= 600 && !faulted; ++t) {
			Result<LogicTickStats> st = rt.step();
			if (!st) {
				faulted = true;
				break;
			}
			CHECK(st->deliveries <= 10000);
			carried_ticks += st->carried_over > 0 ? 1 : 0;
		}
		MESSAGE("Delay 0 cycle: faulted " << faulted << ", ticks with carry-over " << carried_ticks);
		CHECK((faulted || carried_ticks == 600));
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("logic fuzz: random synthetic partitions build or fail within the budget") {
		std::mt19937_64 rng(0xb1e55ed);
		const auto start = std::chrono::steady_clock::now();
		size_t ok = 0, failed = 0;
		for (int round = 0; round < 200; ++round) {
			CAPTURE(round);
			SynPartition s = random_partition(rng, 2 + static_cast<uint32_t>(rng() % 10), static_cast<uint32_t>(rng() % 12));
			EbxDb db;
			const EbxPartition *p = logic_test::insert_syn(db, "levels/synthetic/fuzz", s);
			if (p == nullptr) {
				continue;
			}
			for (const LogicSide side : { LogicSide::Server, LogicSide::Client }) {
				LogicGraphOptions o;
				o.side = side;
				Result<LogicGraph> g = build_logic_graph(db, p->primary(), o);
				if (g) {
					++ok;
					CHECK(g->stats().work <= WorkBudget::kDefaultLimit);
					std::shared_ptr<const LogicGraph> shared = std::make_shared<const LogicGraph>(std::move(*g));
					LogicRuntimeOptions ro;
					run_bounded(shared, rng, 60, ro);
				} else {
					++failed;
				}
			}
		}
		MESSAGE("random partitions: " << ok << " built, " << failed << " failed");
		CHECK(ok > 0);
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("logic fuzz: diamond chains with 2^k paths stop at the fan-out cap or the budget") {
		const auto start = std::chrono::steady_clock::now();
		{
			SynPartition s = diamond_chain(4);
			EbxDb db;
			const EbxPartition *p = logic_test::insert_syn(db, "levels/synthetic/diamond4", s);
			REQUIRE(p != nullptr);
			Result<LogicGraph> g = build_logic_graph(db, p->primary(), {});
			REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
			CHECK(g->event_edges().size() == 16);
			CHECK(g->stats().duplicate_paths == 15);
		}
		{
			SynPartition s = diamond_chain(20);
			EbxDb db;
			const EbxPartition *p = logic_test::insert_syn(db, "levels/synthetic/diamond20", s);
			REQUIRE(p != nullptr);
			Result<LogicGraph> g = build_logic_graph(db, p->primary(), {});
			REQUIRE_FALSE(g.ok());
			MESSAGE("diamond 20 (2 refs per level): " << g.error().message);
		}
		{
			SynPartition s = diamond_chain(10, 1, 2);
			EbxDb db;
			const EbxPartition *p = logic_test::insert_syn(db, "levels/synthetic/parallel10", s);
			REQUIRE(p != nullptr);
			Result<LogicGraph> g = build_logic_graph(db, p->primary(), {});
			REQUIRE_FALSE(g.ok());
			CHECK(g.error().message.find("expands to more than") != std::string::npos);
			MESSAGE("parallel 10: " << g.error().message);
			LogicGraphOptions wide;
			wide.max_fanout = SIZE_MAX;
			Result<LogicGraph> h = build_logic_graph(db, p->primary(), wide);
			REQUIRE_FALSE(h.ok());
			CHECK(h.error().message.find("work budget") != std::string::npos);
			MESSAGE("parallel 10, no fan-out cap: " << h.error().message);
		}
		CHECK(seconds_since(start) < kFastSeconds);
	}

	TEST_CASE("logic fuzz: random ghost bytes and mutated encodings") {
		LogicGraph::Builder b;
		const uint32_t root = b.add_scope(kNoLogicIndex, Guid{}, "root");
		for (uint32_t i = 0; i < 37; ++i) {
			b.add_node(LogicTypeId::SyncedBool, LogicRealm::Server, root, guid_n(i));
		}
		b.add_node(LogicTypeId::SyncedSequence, LogicRealm::Server, root, guid_n(100));
		Result<LogicGraph> built = b.finish();
		REQUIRE(built.ok());
		Result<LogicGhostLayout> layout = LogicGhostLayout::of(*built);
		REQUIRE(layout.ok());
		std::mt19937_64 rng(77);
		size_t accepted = 0;
		LogicGhost g;
		g.bool_count = 37;
		g.bool_bits = { 0x1234567ull };
		g.sequence_start = { LogicGhost::kNoStart };
		const std::vector<uint8_t> good = g.encode();
		REQUIRE(LogicGhost::decode(good, *layout).ok());
		for (int round = 0; round < 20000; ++round) {
			std::vector<uint8_t> bytes;
			if (round % 2 == 0) {
				bytes.resize(rng() % 64);
				for (uint8_t &x : bytes) {
					x = static_cast<uint8_t>(rng());
				}
			} else {
				bytes = good;
				const int flips = 1 + static_cast<int>(rng() % 4);
				for (int k = 0; k < flips; ++k) {
					bytes[rng() % bytes.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
				}
				if (rng() % 5 == 0) {
					bytes.resize(rng() % (bytes.size() + 4));
				}
			}
			Result<LogicGhost> d = LogicGhost::decode(bytes, *layout);
			if (d) {
				++accepted;
				CHECK(d->encode() == bytes);
			}
		}
		MESSAGE("ghost decode accepted " << accepted << " of 20000");
	}
}

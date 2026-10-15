// S4 level logic: the deterministic interpreter (docs/design/s4.md §6.2, §7). See logic_runtime.h for the tick
// contract. Behaviour per primitive type follows the design table (§6.2); every rule marked [inferred] there is
// implemented as written and pinned by logic.unit.
//
// Determinism: one FIFO of event and property items (the value travels in the item), timers in (due tick, arm
// sequence) order, fan-out in edge order, seeds from hash_mix chains of the root seed, the node key and a per-node
// draw count. Nothing iterates a hash container. The trace hash covers every delivery, timer, forced input, effect,
// carry-over and fault.
//
// Hostile graphs: the queue, the timer heap, the effects and the watch hits of a tick are capped (fault); deliveries
// per tick are capped (carry-over); Delay and Sequence times are clamped by logic_delay_ticks; EventSwitch and
// RandomMultiEvent use at most kMaxOutputs outputs; a looping Sequence of length 0 does not restart in the same tick.
#include "bf3/sim/logic/logic_runtime.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/ids.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace bf3::sim::logic {

namespace gp = core::gameplay;
using gp::LogicEdge;
using gp::LogicNode;
using gp::LogicTypeClass;
using gp::LogicTypeId;
using gp::kNoLogicIndex;
using K = LogicValue::Kind;
using T = LogicTypeId;

namespace {

namespace port {
using namespace gp::logic_port;
constexpr int32_t kImpulse = gp::logic_port_id("Impulse");
constexpr int32_t kOnImpulse = gp::logic_port_id("OnImpulse");
constexpr int32_t kRestart = gp::logic_port_id("Restart");
constexpr int32_t kToggle = gp::logic_port_id("Toggle");
constexpr int32_t kIncrement = gp::logic_port_id("Increment");
constexpr int32_t kDecrement = gp::logic_port_id("Decrement");
constexpr int32_t kResetCount = gp::logic_port_id("ResetCount");
constexpr int32_t kSubtractCount = gp::logic_port_id("SubtractCount");
constexpr int32_t kOnReachedZero = gp::logic_port_id("OnReachedZero");
constexpr int32_t kNextOut = gp::logic_port_id("NextOut");
constexpr int32_t kPreviousOut = gp::logic_port_id("PreviousOut");
constexpr int32_t kPause = gp::logic_port_id("Pause");
constexpr int32_t kOnStarted = gp::logic_port_id("OnStarted");
constexpr int32_t kOnTriggerOnlyForPlayer = gp::logic_port_id("OnTriggerOnlyForPlayer");
constexpr int32_t kA = gp::logic_port_id("A");
constexpr int32_t kB = gp::logic_port_id("B");
constexpr int32_t kBool = gp::logic_port_id("Bool");
constexpr int32_t kIntIn0 = gp::logic_port_id("IntIn0");
constexpr int32_t kIntIn1 = gp::logic_port_id("IntIn1");
constexpr int32_t kFloatIn0 = gp::logic_port_id("FloatIn0");
constexpr int32_t kFloatIn1 = gp::logic_port_id("FloatIn1");
constexpr int32_t kOperator = gp::logic_port_id("Operator");
constexpr int32_t kInputSelect = gp::logic_port_id("InputSelect");
constexpr int32_t kCastToBool = gp::logic_port_id("CastToBool");
constexpr int32_t kCastToInt = gp::logic_port_id("CastToInt");
constexpr int32_t kCastToFloat = gp::logic_port_id("CastToFloat");
constexpr int32_t kBoolValue = gp::logic_port_id("BoolValue");
constexpr int32_t kIntValue = gp::logic_port_id("IntValue");
constexpr int32_t kFloatValue = gp::logic_port_id("FloatValue");
constexpr int32_t kBoolIn = gp::logic_port_id("BoolIn");
constexpr int32_t kBoolOut = gp::logic_port_id("BoolOut");
constexpr int32_t kFloatIn = gp::logic_port_id("FloatIn");
constexpr int32_t kFloatOut = gp::logic_port_id("FloatOut");
constexpr int32_t kIntIn = gp::logic_port_id("IntIn");
constexpr int32_t kIntOut = gp::logic_port_id("IntOut");
constexpr int32_t kTransformIn = gp::logic_port_id("TransformIn");
constexpr int32_t kTransformOut = gp::logic_port_id("TransformOut");
constexpr int32_t kVec3In = gp::logic_port_id("Vec3In");
constexpr int32_t kVec3Out = gp::logic_port_id("Vec3Out");
constexpr int32_t kStringOut = gp::logic_port_id("StringOut");
constexpr int32_t kStringIn0 = gp::logic_port_id("StringIn0");
constexpr int32_t kStringIn1 = gp::logic_port_id("StringIn1");
// SwitchPropertyString's two select events (names not recovered; the hashes differ in the last bit like the
// StringIn0 / StringIn1 pair, so the odd one selects 0) [inferred].
constexpr int32_t kSelectString0 = 263676847;
constexpr int32_t kSelectString1 = 263676846;
constexpr int32_t kPrint = gp::logic_port_id("Print");
constexpr int32_t kTriggerAmount = gp::logic_port_id("TriggerAmount");
constexpr int32_t kDefaultValue = gp::logic_port_id("DefaultValue");
constexpr int32_t kIncDecValue = gp::logic_port_id("IncDecValue");
constexpr int32_t kAutoStart = gp::logic_port_id("AutoStart");
constexpr int32_t kRunOnce = gp::logic_port_id("RunOnce");
constexpr int32_t kRemoveDuplicateEvents = gp::logic_port_id("RemoveDuplicateEvents");
constexpr int32_t kMinDelay = gp::logic_port_id("MinDelay");
constexpr int32_t kMaxDelay = gp::logic_port_id("MaxDelay");
constexpr int32_t kDefault = gp::logic_port_id("Default");
constexpr int32_t kTeam = gp::logic_port_id("Team");
constexpr int32_t kInvertFilter = gp::logic_port_id("InvertFilter");
constexpr int32_t kInitalEvent = gp::logic_port_id("InitalEvent");
constexpr int32_t kTriggerOnPropertyChange = gp::logic_port_id("TriggerOnPropertyChange");
constexpr int32_t kTriggerOnStart = gp::logic_port_id("TriggerOnStart");
constexpr int32_t kOutEvents = gp::logic_port_id("OutEvents");
constexpr int32_t kAutoIncrement = gp::logic_port_id("AutoIncrement");
constexpr int32_t kRandomEventWeight = gp::logic_port_id("RandomEventWeight");
constexpr int32_t kUniformDistribution = gp::logic_port_id("UniformDistribution");
constexpr int32_t kDisableOutputOnTrigger = gp::logic_port_id("DisableOutputOnTrigger");
constexpr int32_t kResetOutputsWhenAllHasTriggered = gp::logic_port_id("ResetOutputsWhenAllHasTriggered");
constexpr int32_t kSequenceLength = gp::logic_port_id("SequenceLength");
constexpr int32_t kLooping = gp::logic_port_id("Looping");
constexpr int32_t kTime = gp::logic_port_id("Time");
constexpr int32_t kEventId = gp::logic_port_id("Event.Id");
constexpr int32_t kOperators = gp::logic_port_id("Operators");
constexpr int32_t kStringProperties = gp::logic_port_id("StringProperties");
constexpr int32_t kBoolSettingName = gp::logic_port_id("BoolSettingName");
constexpr int32_t kIntSettingName = gp::logic_port_id("IntSettingName");
constexpr int32_t kFloatSettingName = gp::logic_port_id("FloatSettingName");
constexpr int32_t kTransform = gp::logic_port_id("Transform");
} // namespace port

// Out0..Out9 and In0..In9 (EventSwitch, RandomMultiEvent, IntHub, MathOp).
constexpr size_t kMaxOutputs = 10;
constexpr int32_t kOutN[kMaxOutputs] = { gp::logic_port_id("Out0"), gp::logic_port_id("Out1"), gp::logic_port_id("Out2"), gp::logic_port_id("Out3"),
	gp::logic_port_id("Out4"), gp::logic_port_id("Out5"), gp::logic_port_id("Out6"), gp::logic_port_id("Out7"), gp::logic_port_id("Out8"),
	gp::logic_port_id("Out9") };
constexpr int32_t kInN[kMaxOutputs] = { gp::logic_port_id("In0"), gp::logic_port_id("In1"), gp::logic_port_id("In2"), gp::logic_port_id("In3"),
	gp::logic_port_id("In4"), gp::logic_port_id("In5"), gp::logic_port_id("In6"), gp::logic_port_id("In7"), gp::logic_port_id("In8"),
	gp::logic_port_id("In9") };

int index_of(const int32_t (&p_ids)[kMaxOutputs], int32_t p_port) {
	for (size_t k = 0; k < kMaxOutputs; ++k) {
		if (p_ids[k] == p_port) {
			return static_cast<int>(k);
		}
	}
	return -1;
}

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

bool as_bool(const LogicValue &p_v) {
	switch (p_v.kind) {
		case K::Bool:
			return p_v.b;
		case K::Int:
		case K::Team:
			return p_v.i != 0;
		case K::Float:
			return p_v.f != 0.0;
		default:
			return false;
	}
}

int64_t clamp_to_i64(double p_f) {
	if (!(p_f == p_f)) {
		return 0;
	}
	if (p_f >= 9.0e18) {
		return std::numeric_limits<int64_t>::max();
	}
	if (p_f <= -9.0e18) {
		return std::numeric_limits<int64_t>::min();
	}
	return static_cast<int64_t>(p_f);
}

int64_t as_int(const LogicValue &p_v) {
	switch (p_v.kind) {
		case K::Bool:
			return p_v.b ? 1 : 0;
		case K::Int:
		case K::Team:
		case K::Text:
			return p_v.i;
		case K::Float:
			return clamp_to_i64(p_v.f);
		default:
			return 0;
	}
}

double as_float(const LogicValue &p_v) {
	switch (p_v.kind) {
		case K::Bool:
			return p_v.b ? 1.0 : 0.0;
		case K::Int:
		case K::Team:
			return static_cast<double>(p_v.i);
		case K::Float:
			return p_v.f;
		default:
			return 0.0;
	}
}

// Wrapping arithmetic on hostile integers (no signed overflow).
int64_t wrap_add(int64_t p_a, int64_t p_b) {
	return static_cast<int64_t>(static_cast<uint64_t>(p_a) + static_cast<uint64_t>(p_b));
}

int64_t wrap_mul(int64_t p_a, int64_t p_b) {
	return static_cast<int64_t>(static_cast<uint64_t>(p_a) * static_cast<uint64_t>(p_b));
}

uint64_t mix_chain(uint64_t p_prev, uint64_t p_word) {
	return core::hash_mix(core::hash_mix(p_prev ^ 0xD6E8FEB86659FD93ull) ^ p_word);
}

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

struct PortValue {
	int32_t port = 0;
	LogicValue value;
};

enum class TimerWhat : uint8_t { Out, SequenceEvent, SequenceEnd };

struct Timer {
	Tick due = 0;
	uint64_t seq = 0;
	uint32_t node = 0;
	uint32_t gen = 0;
	TimerWhat what = TimerWhat::Out;
	int32_t aux = 0;
	LogicContext context;
};

struct TimerLater {
	bool operator()(const Timer &a, const Timer &b) const { return a.due != b.due ? a.due > b.due : a.seq > b.seq; }
};

struct Item {
	uint32_t source = kNoLogicIndex;
	int32_t source_port = 0;
	uint32_t target = 0;
	int32_t target_port = 0;
	bool property = false;
	LogicContext context;
	LogicValue value;
};

struct NodeState {
	LogicValue value; // Bool, SyncedBool, Int, Float, BoolToEvent: the current value
	bool flag = false; // EventGate / PropertyGate open, DelayTrigger / MultipleTrigger enabled, Sequence playing
	bool done = false; // RunOnce spent
	uint32_t count = 0; // MultipleTrigger count, EventSwitch / SwitchPropertyString index
	uint32_t pending = 0; // armed timers (Delay)
	uint32_t gen = 0; // timer generation: cancel = ++gen
	uint32_t draws = 0; // seeded draws taken
	Tick start = 0; // Sequence start tick
	uint64_t used = 0; // RandomMultiEvent outputs fired (DisableOutputOnTrigger)
};

} // namespace

struct LogicRuntime::State {
	std::shared_ptr<const LogicGraph> graph;
	LogicRuntimeOptions options;
	LogicGhostLayout layout;
	std::vector<NodeState> nodes;
	std::vector<std::vector<PortValue>> inputs; // received property inputs per node
	std::vector<std::vector<PortValue>> outputs; // published property outputs per node
	std::vector<Item> queue;
	size_t head = 0;
	std::vector<Timer> timers; // heap
	uint64_t timer_seq = 0;
	struct Forced {
		uint32_t node = 0;
		int32_t port = 0;
		bool property = false;
		LogicContext context;
		LogicValue value;
	};
	std::vector<Forced> forced;
	struct Watch {
		uint32_t node = 0;
		int32_t port = 0;
		LogicWatchKind kind = LogicWatchKind::EventIn;
	};
	std::vector<Watch> watches;
	std::vector<uint8_t> watch_bits; // per node: bit 0 EventIn, bit 1 EventOut
	std::vector<LogicEffect> effects;
	std::vector<LogicNetworkedEvent> networked;
	std::vector<LogicWatchHit> hits;
	std::vector<LogicUnhandledPort> unhandled;
	LogicTrace trace;
	Tick tick = 0;
	uint32_t seq = 0; // trace sequence within the tick
	bool started = false;
	// Tick 0's first drain (the init settle): values propagate, but value-change events (OnTrue / OnFalse of Bool,
	// SyncedBool and BoolToEvent, Compare* TriggerOnPropertyChange) do not fire; the settled value is the initial
	// one [inferred, s4.md §7].
	bool settling = false;
	std::optional<core::Error> fault;
	LogicTickStats stats; // the tick being run
	size_t tick_deliveries = 0;

	const LogicGraph &g() const { return *graph; }
	const LogicNode &node(uint32_t p_node) const { return graph->nodes()[p_node]; }

	// --- trace -----------------------------------------------------------------------------------------------------

	uint32_t record(LogicTraceKind p_kind, uint32_t p_source, int32_t p_source_port, uint32_t p_target, int32_t p_target_port, uint64_t p_value_hash,
			int32_t p_player) {
		const uint32_t at = seq++;
		uint8_t b[40] = {};
		const auto put32 = [&](size_t p_at, uint32_t p_v) {
			for (int i = 0; i < 4; ++i) {
				b[p_at + i] = static_cast<uint8_t>(p_v >> (8 * i));
			}
		};
		put32(0, tick);
		put32(4, at);
		b[8] = static_cast<uint8_t>(p_kind);
		put32(12, p_source);
		put32(16, static_cast<uint32_t>(p_source_port));
		put32(20, p_target);
		put32(24, static_cast<uint32_t>(p_target_port));
		for (int i = 0; i < 8; ++i) {
			b[28 + i] = static_cast<uint8_t>(p_value_hash >> (8 * i));
		}
		put32(36, static_cast<uint32_t>(p_player));
		uint64_t h = trace.hash;
		for (const uint8_t byte : b) {
			h ^= byte;
			h *= kFnvPrime;
		}
		trace.hash = h;
		++trace.records_total;
		if (options.keep_trace_records) {
			if (trace.records.size() < options.max_trace_records) {
				LogicTraceRecord r;
				r.tick = tick;
				r.seq = at;
				r.kind = p_kind;
				r.source = p_source;
				r.source_port = p_source_port;
				r.target = p_target;
				r.target_port = p_target_port;
				r.value_hash = p_value_hash;
				r.player = p_player;
				trace.records.push_back(r);
			} else {
				trace.truncated = true;
			}
		}
		return at;
	}

	void set_fault(std::string p_message) {
		if (fault) {
			return;
		}
		fault = core::make_error("logic runtime: tick " + std::to_string(tick) + ": " + p_message);
		record(LogicTraceKind::Fault, kNoLogicIndex, 0, kNoLogicIndex, 0, 0, -1);
	}

	// --- params and ports -----------------------------------------------------------------------------------------

	std::optional<LogicValue> param(uint32_t p_node, int32_t p_field, uint16_t p_element = 0) const { return graph->param(p_node, p_field, p_element); }
	bool param_bool(uint32_t p_node, int32_t p_field, bool p_default) const {
		const std::optional<LogicValue> v = param(p_node, p_field);
		return v ? as_bool(*v) : p_default;
	}
	size_t param_count(uint32_t p_node, int32_t p_field) const {
		size_t n = 0;
		for (const gp::LogicParam &p : graph->params_of(p_node)) {
			n += p.field == p_field;
		}
		return n;
	}

	static PortValue *find_port(std::vector<PortValue> &r_list, int32_t p_port) {
		for (PortValue &pv : r_list) {
			if (pv.port == p_port) {
				return &pv;
			}
		}
		return nullptr;
	}

	// A property input: the last value delivered, else the node's param of that id, else None.
	LogicValue in(uint32_t p_node, int32_t p_port) const {
		for (const PortValue &pv : inputs[p_node]) {
			if (pv.port == p_port) {
				return pv.value;
			}
		}
		if (const std::optional<LogicValue> v = param(p_node, p_port)) {
			return *v;
		}
		return LogicValue{};
	}
	bool has_input(uint32_t p_node, int32_t p_port) const {
		for (const PortValue &pv : inputs[p_node]) {
			if (pv.port == p_port) {
				return true;
			}
		}
		return false;
	}

	void unhandled_port(uint32_t p_node, int32_t p_port, bool p_event) {
		const LogicTypeId type = node(p_node).type;
		for (LogicUnhandledPort &u : unhandled) {
			if (u.type == type && u.port == p_port && u.event == p_event) {
				++u.count;
				return;
			}
		}
		if (unhandled.size() < kMaxUnhandledPorts) {
			unhandled.push_back({ type, p_port, p_event, 1 });
		}
	}

	// --- queue, fan-out, effects ----------------------------------------------------------------------------------

	bool enqueue(const Item &p_item) {
		if (queue.size() - head >= options.max_queue) {
			set_fault("queue holds more than " + std::to_string(options.max_queue) + " items");
			return false;
		}
		if (head > 4096 && head * 2 > queue.size()) {
			queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(head));
			head = 0;
		}
		queue.push_back(p_item);
		stats.queue_peak = std::max(stats.queue_peak, queue.size() - head);
		return true;
	}

	void watch_hit(uint32_t p_node, int32_t p_port, LogicWatchKind p_kind, const LogicContext &p_context, uint32_t p_seq) {
		const uint8_t bit = p_kind == LogicWatchKind::EventIn ? 1 : 2;
		if ((watch_bits[p_node] & bit) == 0) {
			return;
		}
		for (const Watch &w : watches) {
			if (w.node == p_node && w.port == p_port && w.kind == p_kind) {
				if (hits.size() >= options.max_watch_hits_per_tick) {
					set_fault("more than " + std::to_string(options.max_watch_hits_per_tick) + " watch hits in one tick");
					return;
				}
				LogicWatchHit h;
				h.tick = tick;
				h.seq = p_seq;
				h.node = p_node;
				h.port = p_port;
				h.kind = p_kind;
				h.context = p_context;
				hits.push_back(h);
				++stats.watch_hits;
				return;
			}
		}
	}

	// The edges of a node's list leaving port p_port (the list is sorted by source port, then order).
	static std::span<const LogicEdge> port_range(std::span<const LogicEdge> p_edges, int32_t p_port) {
		const auto lo = std::lower_bound(p_edges.begin(), p_edges.end(), p_port, [](const LogicEdge &e, int32_t p) { return e.source_port < p; });
		auto hi = lo;
		while (hi != p_edges.end() && hi->source_port == p_port) {
			++hi;
		}
		return std::span<const LogicEdge>(lo, hi);
	}

	// Node p_node fires event p_port: every event edge of that port is queued in edge order; the networked-only
	// edges are reported for clients.
	void fire(uint32_t p_node, int32_t p_port, const LogicContext &p_context) {
		if (fault) {
			return;
		}
		watch_hit(p_node, p_port, LogicWatchKind::EventOut, p_context, seq);
		for (const LogicEdge &e : port_range(graph->events_from(p_node), p_port)) {
			Item it;
			it.source = p_node;
			it.source_port = p_port;
			it.target = e.target;
			it.target_port = e.target_port;
			it.context = p_context;
			if (!enqueue(it)) {
				return;
			}
			if (e.networked) {
				report_networked(e, p_context);
			}
		}
		const std::span<const LogicEdge> all = graph->networked_edges();
		const auto lo = std::lower_bound(all.begin(), all.end(), std::make_pair(p_node, p_port), [](const LogicEdge &e, const std::pair<uint32_t, int32_t> &k) {
			return e.source != k.first ? e.source < k.first : e.source_port < k.second;
		});
		for (auto it = lo; it != all.end() && it->source == p_node && it->source_port == p_port; ++it) {
			report_networked(*it, p_context);
		}
	}

	void report_networked(const LogicEdge &p_edge, const LogicContext &p_context) {
		LogicNetworkedEvent ev;
		ev.tick = tick;
		ev.connection = p_edge.connection;
		ev.source = p_edge.source;
		ev.source_port = p_edge.source_port;
		ev.context = p_context;
		networked.push_back(ev);
		++stats.networked;
	}

	// A property output: queued along its edges only when the value changes (bitwise).
	void publish(uint32_t p_node, int32_t p_port, const LogicValue &p_value) {
		if (fault) {
			return;
		}
		PortValue *out = find_port(outputs[p_node], p_port);
		if (out != nullptr) {
			if (out->value.same_as(p_value)) {
				return;
			}
			out->value = p_value;
		} else {
			outputs[p_node].push_back({ p_port, p_value });
		}
		for (const LogicEdge &e : port_range(graph->properties_from(p_node), p_port)) {
			Item it;
			it.source = p_node;
			it.source_port = p_port;
			it.target = e.target;
			it.target_port = e.target_port;
			it.property = true;
			it.value = p_value;
			if (!enqueue(it)) {
				return;
			}
		}
	}

	// Publishes p_value on every port the node has property edges from (types whose output port name is not
	// recovered: Locator, Setting).
	void publish_all_ports(uint32_t p_node, const LogicValue &p_value) {
		const std::span<const LogicEdge> edges = graph->properties_from(p_node);
		for (size_t k = 0; k < edges.size(); ++k) {
			if (k == 0 || edges[k].source_port != edges[k - 1].source_port) {
				publish(p_node, edges[k].source_port, p_value);
			}
		}
	}

	void effect(const Item &p_item) {
		if (effects.size() >= options.max_effects_per_tick) {
			set_fault("more than " + std::to_string(options.max_effects_per_tick) + " native effects in one tick");
			return;
		}
		LogicEffect e;
		e.apply_tick = tick + 1;
		e.node = p_item.target;
		e.port = p_item.target_port;
		e.kind = p_item.property ? LogicEffectKind::Property : LogicEffectKind::Event;
		e.value = p_item.value;
		e.context = p_item.context;
		effects.push_back(e);
		++stats.effects;
		record(LogicTraceKind::Effect, p_item.source, p_item.source_port, p_item.target, p_item.target_port,
				p_item.property ? gp::logic_value_hash(p_item.value) : 0, p_item.context.player);
	}

	// --- timers ---------------------------------------------------------------------------------------------------

	// Arms a timer p_ticks from now; 0 runs it at once (same drain).
	void arm(uint32_t p_node, uint32_t p_ticks, TimerWhat p_what, int32_t p_aux, const LogicContext &p_context) {
		if (fault) {
			return;
		}
		NodeState &st = nodes[p_node];
		if (p_what == TimerWhat::Out) {
			++st.pending;
		}
		Timer t;
		t.due = tick + p_ticks;
		if (t.due < tick) {
			t.due = std::numeric_limits<Tick>::max(); // saturate
		}
		t.seq = timer_seq++;
		t.node = p_node;
		t.gen = st.gen;
		t.what = p_what;
		t.aux = p_aux;
		t.context = p_context;
		if (p_ticks == 0) {
			run_timer(t);
			return;
		}
		if (timers.size() >= options.max_timers) {
			set_fault("more than " + std::to_string(options.max_timers) + " armed timers");
			return;
		}
		timers.push_back(t);
		std::push_heap(timers.begin(), timers.end(), TimerLater{});
	}

	void cancel(uint32_t p_node) {
		NodeState &st = nodes[p_node];
		++st.gen;
		st.pending = 0;
	}

	void run_timer(const Timer &p_timer) {
		NodeState &st = nodes[p_timer.node];
		if (p_timer.gen != st.gen) {
			return; // cancelled
		}
		++stats.timers_fired;
		record(LogicTraceKind::TimerFired, p_timer.node, static_cast<int32_t>(p_timer.what), p_timer.node, p_timer.aux, 0, p_timer.context.player);
		const T type = node(p_timer.node).type;
		switch (p_timer.what) {
			case TimerWhat::Out:
				if (st.pending > 0) {
					--st.pending;
				}
				switch (type) {
					case T::Delay:
					case T::RandomDelay:
						fire(p_timer.node, port::kOut, p_timer.context);
						break;
					case T::DelayTrigger:
					case T::MultipleTrigger:
						fire(p_timer.node, port::kOnTrigger, p_timer.context);
						break;
					default:
						break;
				}
				break;
			case TimerWhat::SequenceEvent:
				if (const std::optional<LogicValue> id = param(p_timer.node, port::kEventId, static_cast<uint16_t>(p_timer.aux))) {
					fire(p_timer.node, static_cast<int32_t>(as_int(*id)), p_timer.context);
				}
				break;
			case TimerWhat::SequenceEnd:
				st.flag = false;
				fire(p_timer.node, port::kOnFinished, p_timer.context);
				if (param_bool(p_timer.node, port::kLooping, false) && p_timer.aux > 0) {
					sequence_start(p_timer.node, p_timer.context);
				}
				break;
		}
	}

	void run_due_timers() {
		while (!timers.empty() && !fault && timers.front().due <= tick) {
			std::pop_heap(timers.begin(), timers.end(), TimerLater{});
			const Timer t = timers.back();
			timers.pop_back();
			run_timer(t);
		}
	}

	// --- per-type behaviour ---------------------------------------------------------------------------------------

	double delay_seconds(uint32_t p_node) {
		const T type = node(p_node).type;
		if (type == T::RandomDelay) {
			const double lo = as_float(in(p_node, port::kMinDelay));
			const double hi = as_float(in(p_node, port::kMaxDelay));
			NodeState &st = nodes[p_node];
			const uint64_t h = mix_chain(mix_chain(options.root_seed, node(p_node).key), st.draws++);
			const double u = static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
			return lo + u * (hi - lo);
		}
		return as_float(in(p_node, port::kDelay));
	}

	void delay_in(uint32_t p_node, const LogicContext &p_context) {
		NodeState &st = nodes[p_node];
		const bool run_once = param_bool(p_node, port::kRunOnce, false);
		if (run_once && st.done) {
			return;
		}
		if (node(p_node).type == T::Delay && param_bool(p_node, port::kRemoveDuplicateEvents, false) && st.pending > 0) {
			return;
		}
		st.done = st.done || run_once;
		arm(p_node, logic_delay_ticks(delay_seconds(p_node), options.hz), TimerWhat::Out, 0, p_context);
	}

	void set_bool(uint32_t p_node, bool p_value, int32_t p_out, const LogicContext &p_context) {
		NodeState &st = nodes[p_node];
		if (st.value.kind == K::Bool && st.value.b == p_value) {
			return;
		}
		st.value = v_bool(p_value);
		publish(p_node, p_out, st.value);
		if (!settling) {
			fire(p_node, p_value ? port::kOnTrue : port::kOnFalse, p_context);
		}
	}

	void sequence_start(uint32_t p_node, const LogicContext &p_context) {
		cancel(p_node);
		NodeState &st = nodes[p_node];
		st.flag = true;
		st.start = tick;
		fire(p_node, port::kOnStarted, p_context);
		const size_t events = std::min<size_t>(param_count(p_node, port::kTime), gp::kMaxParamElements);
		for (size_t k = 0; k < events && !fault; ++k) {
			const std::optional<LogicValue> time = param(p_node, port::kTime, static_cast<uint16_t>(k));
			const double ms = time ? as_float(*time) : 0.0;
			arm(p_node, logic_delay_ticks(ms / 1000.0, options.hz), TimerWhat::SequenceEvent, static_cast<int32_t>(k), p_context);
		}
		const uint32_t length = logic_delay_ticks(as_float(in(p_node, port::kSequenceLength)) / 1000.0, options.hz);
		// aux = the length in ticks (0: a looping sequence does not restart in the tick it ended).
		arm(p_node, length, TimerWhat::SequenceEnd, static_cast<int32_t>(std::min<uint32_t>(length, 0x7fffffff)), p_context);
	}

	void sequence_stop(uint32_t p_node, bool p_notify, const LogicContext &p_context) {
		NodeState &st = nodes[p_node];
		const bool was = st.flag;
		cancel(p_node);
		st.flag = false;
		if (was && p_notify) {
			fire(p_node, port::kOnStopped, p_context);
		}
	}

	// CompareInt / CompareFloat: every outcome that holds fires, in a fixed order.
	void compare_fire(uint32_t p_node, const LogicContext &p_context) {
		const bool is_int = node(p_node).type == T::CompareInt;
		const LogicValue a = in(p_node, port::kA), b = in(p_node, port::kB);
		int c = 0;
		if (is_int) {
			const int64_t x = as_int(a), y = as_int(b);
			c = x < y ? -1 : (x > y ? 1 : 0);
		} else {
			const double x = as_float(a), y = as_float(b);
			c = x < y ? -1 : (x > y ? 1 : (x == y ? 0 : 2)); // 2: unordered (NaN), nothing holds
		}
		struct Outcome {
			int32_t port;
			bool holds;
		};
		const Outcome outcomes[] = {
			{ gp::logic_port_id("A<B"), c == -1 },
			{ gp::logic_port_id("A<=B"), c == -1 || c == 0 },
			{ gp::logic_port_id("A=B"), c == 0 },
			{ gp::logic_port_id("A!=B"), c != 0 && c != 2 },
			{ gp::logic_port_id("A>=B"), c == 1 || c == 0 },
			{ gp::logic_port_id("A>B"), c == 1 },
		};
		for (const Outcome &o : outcomes) {
			if (o.holds) {
				fire(p_node, o.port, p_context);
			}
		}
	}

	bool compare_out(uint32_t p_node) {
		const LogicValue op = in(p_node, port::kOperator);
		const std::string_view name = op.kind == K::Text ? graph->text(static_cast<uint32_t>(op.i)) : std::string_view();
		const bool floats = has_input(p_node, port::kFloatIn0) || has_input(p_node, port::kFloatIn1);
		int c = 0;
		if (floats) {
			const double x = as_float(in(p_node, port::kFloatIn0)), y = as_float(in(p_node, port::kFloatIn1));
			c = x < y ? -1 : (x > y ? 1 : (x == y ? 0 : 2));
		} else {
			const int64_t x = as_int(in(p_node, port::kIntIn0)), y = as_int(in(p_node, port::kIntIn1));
			c = x < y ? -1 : (x > y ? 1 : 0);
		}
		if (c == 2) {
			return false;
		}
		if (name == "CompareOp_Equal") {
			return c == 0;
		}
		if (name == "CompareOp_NotEqual") {
			return c != 0;
		}
		if (name == "CompareOp_Less") {
			return c < 0;
		}
		if (name == "CompareOp_LessOrEqual") {
			return c <= 0;
		}
		if (name == "CompareOp_Greater") {
			return c > 0;
		}
		if (name == "CompareOp_GreaterOrEqual") {
			return c >= 0;
		}
		return false;
	}

	// MathOp: Out = In0 op0 In1 op1 In2 ... left to right; operators 0 add, 1 subtract, 2 multiply, 3 divide (0 on a
	// zero divisor), 4 min, 5 max [inferred: the enumerator names are not in the data]. Integer inputs stay integer.
	LogicValue math_out(uint32_t p_node) {
		const size_t ops = std::min(param_count(p_node, port::kOperators), kMaxOutputs - 1);
		LogicValue acc = in(p_node, kInN[0]);
		bool floats = acc.kind == K::Float;
		double f = as_float(acc);
		int64_t i = as_int(acc);
		for (size_t k = 0; k < ops; ++k) {
			const LogicValue rhs = in(p_node, kInN[k + 1]);
			floats = floats || rhs.kind == K::Float;
			const double rf = as_float(rhs);
			const int64_t ri = as_int(rhs);
			const std::optional<LogicValue> opv = param(p_node, port::kOperators, static_cast<uint16_t>(k));
			switch (opv ? as_int(*opv) : 0) {
				case 1:
					f -= rf;
					i = wrap_add(i, -static_cast<int64_t>(static_cast<uint64_t>(ri)));
					break;
				case 2:
					f *= rf;
					i = wrap_mul(i, ri);
					break;
				case 3:
					f = rf != 0.0 ? f / rf : 0.0;
					i = (ri != 0 && !(i == std::numeric_limits<int64_t>::min() && ri == -1)) ? i / ri : 0;
					break;
				case 4:
					f = std::min(f, rf);
					i = std::min(i, ri);
					break;
				case 5:
					f = std::max(f, rf);
					i = std::max(i, ri);
					break;
				default:
					f += rf;
					i = wrap_add(i, ri);
					break;
			}
		}
		return floats ? v_float(f) : v_int(i);
	}

	void on_event(uint32_t p_node, int32_t p_port, const LogicContext &p_context) {
		NodeState &st = nodes[p_node];
		switch (node(p_node).type) {
			case T::Delay:
			case T::RandomDelay:
				if (p_port == port::kIn) {
					delay_in(p_node, p_context);
				} else if (p_port == port::kRestart) {
					cancel(p_node);
					arm(p_node, logic_delay_ticks(delay_seconds(p_node), options.hz), TimerWhat::Out, 0, p_context);
				} else if (p_port == port::kStop || p_port == port::kReset) {
					cancel(p_node);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::DelayTrigger:
				if (p_port == port::kEnable) {
					st.flag = true;
				} else if (p_port == port::kDisable) {
					st.flag = false;
				} else if (p_port == port::kReset) {
					st.done = false;
					cancel(p_node);
				} else if (p_port == port::kTrigger) {
					const bool run_once = param_bool(p_node, port::kRunOnce, false);
					if (st.flag && !(run_once && st.done)) {
						st.done = st.done || run_once;
						arm(p_node, logic_delay_ticks(as_float(in(p_node, port::kDelay)), options.hz), TimerWhat::Out, 0, p_context);
					}
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::MultipleTrigger:
				if (p_port == port::kTrigger) {
					if (st.flag && !st.done) {
						++st.count;
						const int64_t amount = std::max<int64_t>(1, as_int(in(p_node, port::kTriggerAmount)));
						if (static_cast<int64_t>(st.count) >= amount) {
							st.count = 0;
							st.done = param_bool(p_node, port::kRunOnce, false);
							arm(p_node, logic_delay_ticks(as_float(in(p_node, port::kDelay)), options.hz), TimerWhat::Out, 0, p_context);
						}
					}
				} else if (p_port == port::kSubtractCount) {
					if (st.count > 0) {
						--st.count;
						if (st.count == 0) {
							fire(p_node, port::kOnReachedZero, p_context);
						}
					}
				} else if (p_port == port::kResetCount) {
					st.count = 0;
				} else if (p_port == port::kEnable) {
					st.flag = true;
				} else if (p_port == port::kDisable) {
					st.flag = false;
				} else if (p_port == port::kReset) {
					st.count = 0;
					st.done = false;
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::EventGate:
				if (p_port == port::kIn) {
					if (st.flag) {
						fire(p_node, port::kOut, p_context);
					}
				} else if (p_port == port::kOpen) {
					st.flag = true;
				} else if (p_port == port::kClose) {
					st.flag = false;
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::EventSplitter:
				if (p_port == port::kImpulse) {
					const bool run_once = param_bool(p_node, port::kRunOnce, false);
					if (!(run_once && st.done)) {
						st.done = run_once;
						fire(p_node, port::kOnImpulse, p_context);
					}
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::EventSwitch: {
				const uint32_t n = static_cast<uint32_t>(std::clamp<int64_t>(as_int(in(p_node, port::kOutEvents)), 1, kMaxOutputs));
				if (p_port == port::kIn) {
					const uint32_t k = st.count % n;
					if (param_bool(p_node, port::kAutoIncrement, false)) {
						st.count = (k + 1) % n;
					}
					fire(p_node, kOutN[k], p_context);
				} else if (p_port == port::kNextOut) {
					st.count = (st.count + 1) % n;
				} else if (p_port == port::kPreviousOut) {
					st.count = (st.count + n - 1) % n;
				} else if (p_port == port::kReset) {
					st.count = 0;
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			}
			case T::RandomMultiEvent: {
				if (p_port != port::kIn) {
					unhandled_port(p_node, p_port, true);
					return;
				}
				const size_t n = std::min(param_count(p_node, port::kRandomEventWeight), kMaxOutputs);
				if (n == 0) {
					return;
				}
				const bool uniform = param_bool(p_node, port::kUniformDistribution, false);
				const bool disable = param_bool(p_node, port::kDisableOutputOnTrigger, false);
				const bool reset_all = param_bool(p_node, port::kResetOutputsWhenAllHasTriggered, false);
				const uint64_t all = (uint64_t(1) << n) - 1;
				if (disable && (st.used & all) == all) {
					if (!reset_all) {
						return;
					}
					st.used = 0;
				}
				double weights[kMaxOutputs] = {};
				double total = 0;
				for (size_t k = 0; k < n; ++k) {
					const std::optional<LogicValue> w = param(p_node, port::kRandomEventWeight, static_cast<uint16_t>(k));
					double x = uniform ? 1.0 : (w ? as_float(*w) : 0.0);
					if (!(x > 0.0) || !std::isfinite(x)) {
						x = 0.0;
					}
					if (disable && (st.used >> k) & 1u) {
						x = 0.0;
					}
					weights[k] = x;
					total += x;
				}
				if (!(total > 0.0)) {
					return;
				}
				const uint64_t h = mix_chain(mix_chain(options.root_seed, node(p_node).key), st.draws++);
				double pick = static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0) * total;
				size_t chosen = n - 1;
				for (size_t k = 0; k < n; ++k) {
					if (weights[k] > 0.0 && pick < weights[k]) {
						chosen = k;
						break;
					}
					pick -= weights[k];
				}
				while (weights[chosen] <= 0.0 && chosen > 0) {
					--chosen;
				}
				if (disable) {
					st.used |= uint64_t(1) << chosen;
					if (reset_all && (st.used & all) == all) {
						st.used = 0;
					}
				}
				fire(p_node, kOutN[chosen], p_context);
				return;
			}
			case T::Sequence:
			case T::SyncedSequence:
				if (p_port == port::kStart) {
					sequence_start(p_node, p_context);
				} else if (p_port == port::kStop) {
					sequence_stop(p_node, true, p_context);
				} else if (p_port == port::kReset || p_port == port::kPause) {
					sequence_stop(p_node, false, p_context);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::TeamFilter:
				if (p_port == port::kIn) {
					const int64_t team = as_int(in(p_node, port::kTeam));
					bool match = p_context.team == team;
					if (param_bool(p_node, port::kInvertFilter, false)) {
						match = !match;
					}
					if (match) {
						fire(p_node, port::kOnTriggeredByMatchingTeam, p_context);
					}
					// "Only for the specified team": fires whatever the cause, addressed to that team [inferred].
					LogicContext addressed = p_context;
					addressed.team = static_cast<uint8_t>(std::clamp<int64_t>(team, 0, 255));
					fire(p_node, port::kOnTriggerOnlyForSpecifiedTeam, addressed);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::PlayerFilter:
				if (p_port == port::kIn) {
					bool pass = p_context.player >= 0;
					if (param_bool(p_node, port::kInvertFilter, false)) {
						pass = !pass;
					}
					if (pass) {
						fire(p_node, port::kOnTriggerOnlyForPlayer, p_context);
					}
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::Bool:
			case T::SyncedBool: {
				const int32_t out = node(p_node).type == T::Bool ? port::kValue : port::kOut;
				if (p_port == port::kSetTrue) {
					set_bool(p_node, true, out, p_context);
				} else if (p_port == port::kSetFalse) {
					set_bool(p_node, false, out, p_context);
				} else if (p_port == port::kToggle) {
					set_bool(p_node, !as_bool(st.value), out, p_context);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			}
			case T::Int:
			case T::Float: {
				const bool is_int = node(p_node).type == T::Int;
				if (p_port == port::kIncrement || p_port == port::kDecrement) {
					const LogicValue step = in(p_node, port::kIncDecValue);
					const bool up = p_port == port::kIncrement;
					st.value = is_int ? v_int(wrap_add(as_int(st.value), up ? as_int(step) : -static_cast<int64_t>(static_cast<uint64_t>(as_int(step)))))
									  : v_float(as_float(st.value) + (up ? as_float(step) : -as_float(step)));
				} else if (p_port == port::kReset) {
					const LogicValue d = in(p_node, port::kDefaultValue);
					st.value = is_int ? v_int(as_int(d)) : v_float(as_float(d));
				} else {
					unhandled_port(p_node, p_port, true);
					return;
				}
				publish(p_node, port::kValue, st.value);
				return;
			}
			case T::CompareInt:
			case T::CompareFloat:
				if (p_port == port::kIn) {
					compare_fire(p_node, p_context);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::CompareBool:
				if (p_port == port::kIn) {
					fire(p_node, as_bool(in(p_node, port::kBool)) ? port::kOnTrue : port::kOnFalse, p_context);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::PropertyGate:
				if (p_port == port::kOpen) {
					st.flag = true;
					property_gate_publish(p_node, 0);
				} else if (p_port == port::kClose) {
					st.flag = false;
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::SwitchPropertyString:
				if (p_port == port::kSelectString0 || p_port == port::kSelectString1) {
					st.count = p_port == port::kSelectString0 ? 0 : 1;
					switch_string_publish(p_node);
				} else {
					unhandled_port(p_node, p_port, true);
				}
				return;
			case T::PrintDebugText:
				if (p_port != port::kPrint) {
					unhandled_port(p_node, p_port, true);
				}
				return; // the trace record of the delivery is the print
			default:
				unhandled_port(p_node, p_port, true);
				return;
		}
	}

	void property_gate_publish(uint32_t p_node, int32_t p_only) {
		static constexpr std::pair<int32_t, int32_t> kPairs[] = { { port::kBoolIn, port::kBoolOut }, { port::kFloatIn, port::kFloatOut },
			{ port::kIntIn, port::kIntOut }, { port::kTransformIn, port::kTransformOut }, { port::kVec3In, port::kVec3Out } };
		if (!nodes[p_node].flag) {
			return;
		}
		for (const auto &[in_port, out_port] : kPairs) {
			if (p_only != 0 && p_only != in_port) {
				continue;
			}
			const LogicValue v = in(p_node, in_port);
			if (v.kind != K::None) {
				publish(p_node, out_port, v);
			}
		}
	}

	void switch_string_publish(uint32_t p_node) {
		const uint32_t k = nodes[p_node].count;
		const int32_t in_port = k == 0 ? port::kStringIn0 : port::kStringIn1;
		LogicValue v = in(p_node, in_port);
		if (v.kind == K::None) {
			if (const std::optional<LogicValue> p = param(p_node, port::kStringProperties, static_cast<uint16_t>(k))) {
				v = *p;
			}
		}
		if (v.kind != K::None) {
			publish(p_node, port::kStringOut, v);
		}
	}

	// Recomputes and publishes a property node's outputs (on start and on every input).
	void recompute(uint32_t p_node) {
		NodeState &st = nodes[p_node];
		switch (node(p_node).type) {
			case T::Not:
				publish(p_node, port::kOut, v_bool(!as_bool(in(p_node, port::kIn))));
				return;
			case T::And:
				publish(p_node, port::kOut, v_bool(as_bool(in(p_node, port::kIn1)) && as_bool(in(p_node, port::kIn2))));
				return;
			case T::Or:
				publish(p_node, port::kOut, v_bool(as_bool(in(p_node, port::kIn1)) || as_bool(in(p_node, port::kIn2))));
				return;
			case T::Or4:
				publish(p_node, port::kOut,
						v_bool(as_bool(in(p_node, port::kIn1)) || as_bool(in(p_node, port::kIn2)) || as_bool(in(p_node, port::kIn3)) ||
								as_bool(in(p_node, port::kIn4))));
				return;
			case T::Compare:
				publish(p_node, port::kOut, v_bool(compare_out(p_node)));
				return;
			case T::IntHub: {
				const int64_t sel = as_int(in(p_node, port::kInputSelect));
				if (sel >= 0 && sel < static_cast<int64_t>(kMaxOutputs)) {
					publish(p_node, port::kOut, v_int(as_int(in(p_node, kInN[sel]))));
				}
				return;
			}
			case T::MathOp:
				publish(p_node, port::kOut, math_out(p_node));
				return;
			default:
				(void)st;
				return;
		}
	}

	void on_property(uint32_t p_node, int32_t p_port, const LogicValue &p_value) {
		NodeState &st = nodes[p_node];
		const T type = node(p_node).type;
		switch (type) {
			case T::Delay:
			case T::DelayTrigger:
			case T::MultipleTrigger:
				if (p_port != port::kDelay && p_port != port::kTriggerAmount) {
					unhandled_port(p_node, p_port, false);
				}
				return; // read when armed
			case T::SyncedBool:
				if (p_port == port::kIn) {
					set_bool(p_node, as_bool(p_value), port::kOut, LogicContext{});
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			case T::BoolToEvent:
				if (p_port == port::kValue) {
					const bool b = as_bool(p_value);
					if (st.value.kind != K::Bool || st.value.b != b) {
						st.value = v_bool(b);
						if (!settling) {
							fire(p_node, b ? port::kOnTrue : port::kOnFalse, LogicContext{});
						}
					}
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			case T::Not:
			case T::And:
			case T::Or:
			case T::Or4:
			case T::Compare:
			case T::IntHub:
			case T::MathOp:
				recompute(p_node);
				return;
			case T::CompareInt:
			case T::CompareFloat:
				if (p_port == port::kA || p_port == port::kB) {
					if (!settling && param_bool(p_node, port::kTriggerOnPropertyChange, false)) {
						compare_fire(p_node, LogicContext{});
					}
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			case T::CompareBool:
				if (p_port == port::kBool) {
					if (!settling && param_bool(p_node, port::kTriggerOnPropertyChange, false)) {
						fire(p_node, as_bool(p_value) ? port::kOnTrue : port::kOnFalse, LogicContext{});
					}
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			case T::PropertyCast:
				if (p_port == port::kIntValue || p_port == port::kFloatValue || p_port == port::kBoolValue) {
					publish(p_node, port::kCastToBool, v_bool(as_bool(p_value)));
					publish(p_node, port::kCastToInt, v_int(as_int(p_value)));
					publish(p_node, port::kCastToFloat, v_float(as_float(p_value)));
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			case T::PropertyGate:
				property_gate_publish(p_node, p_port);
				return;
			case T::SwitchPropertyString:
				if (p_port == port::kStringIn0 || p_port == port::kStringIn1) {
					if ((p_port == port::kStringIn0) == (st.count == 0)) {
						switch_string_publish(p_node);
					}
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			case T::PrintDebugText:
				return;
			case T::Locator:
				// A driven Transform republishes like the constant one at start (XP3_Valley drives some) [inferred].
				if (p_port == port::kTransform) {
					publish_all_ports(p_node, p_value);
				} else {
					unhandled_port(p_node, p_port, false);
				}
				return;
			default:
				unhandled_port(p_node, p_port, false);
				return;
		}
	}

	void deliver(const Item &p_item) {
		++tick_deliveries;
		++stats.deliveries;
		const uint32_t at = p_item.property
				? record(LogicTraceKind::Property, p_item.source, p_item.source_port, p_item.target, p_item.target_port, gp::logic_value_hash(p_item.value),
						  -1)
				: record(LogicTraceKind::Event, p_item.source, p_item.source_port, p_item.target, p_item.target_port, 0, p_item.context.player);
		if (!p_item.property) {
			watch_hit(p_item.target, p_item.target_port, LogicWatchKind::EventIn, p_item.context, at);
		}
		const LogicTypeClass cls = gp::logic_type_info(node(p_item.target).type).cls;
		if (cls == LogicTypeClass::Native) {
			effect(p_item);
			return;
		}
		if (cls != LogicTypeClass::Primitive) {
			return; // passive, data, structural, unknown: swallowed
		}
		if (p_item.property) {
			if (PortValue *pv = find_port(inputs[p_item.target], p_item.target_port)) {
				pv->value = p_item.value;
			} else {
				inputs[p_item.target].push_back({ p_item.target_port, p_item.value });
			}
			on_property(p_item.target, p_item.target_port, p_item.value);
		} else {
			on_event(p_item.target, p_item.target_port, p_item.context);
		}
	}

	void drain() {
		while (head < queue.size() && !fault) {
			if (tick_deliveries >= options.max_deliveries_per_tick) {
				break;
			}
			const Item item = queue[head++];
			deliver(item);
		}
		if (head == queue.size()) {
			queue.clear();
			head = 0;
		}
	}

	void finish_tick() {
		stats.carried_over = queue.size() - head;
		if (stats.carried_over > 0) {
			record(LogicTraceKind::CarriedOver, kNoLogicIndex, 0, kNoLogicIndex, 0, stats.carried_over, -1);
		}
	}

	void begin_tick() {
		stats = LogicTickStats{};
		stats.tick = tick;
		seq = 0;
		tick_deliveries = 0;
		effects.clear();
		networked.clear();
		hits.clear();
	}

	// --- tick 0 ---------------------------------------------------------------------------------------------------

	void init_states() {
		for (uint32_t i = 0; i < nodes.size(); ++i) {
			NodeState &st = nodes[i];
			switch (node(i).type) {
				case T::EventGate:
				case T::PropertyGate:
					st.flag = param_bool(i, port::kDefault, true);
					break;
				case T::DelayTrigger:
				case T::MultipleTrigger:
					st.flag = param_bool(i, port::kEnabled, true);
					break;
				case T::Bool:
					st.value = v_bool(param_bool(i, port::kDefaultValue, false));
					break;
				case T::SyncedBool:
					st.value = v_bool(param_bool(i, port::kIn, false));
					break;
				case T::Int:
					st.value = v_int(as_int(in(i, port::kDefaultValue)));
					break;
				case T::Float:
					st.value = v_float(as_float(in(i, port::kDefaultValue)));
					break;
				case T::BoolToEvent:
					st.value = v_bool(param_bool(i, port::kValue, false));
					break;
				default:
					break;
			}
		}
	}

	void publish_initial(uint32_t p_node) {
		NodeState &st = nodes[p_node];
		switch (node(p_node).type) {
			case T::Bool:
			case T::Int:
			case T::Float:
				publish(p_node, port::kValue, st.value);
				return;
			case T::SyncedBool:
				publish(p_node, port::kOut, st.value);
				return;
			case T::Not:
			case T::And:
			case T::Or:
			case T::Or4:
			case T::Compare:
			case T::IntHub:
			case T::MathOp:
				recompute(p_node);
				return;
			case T::PropertyGate:
				property_gate_publish(p_node, 0);
				return;
			case T::SwitchPropertyString:
				switch_string_publish(p_node);
				return;
			case T::Setting: {
				// Server settings table: S4 has none, so every bool setting reads true (e.g. Server.VehicleSpawnAllowed).
				const std::optional<LogicValue> name = param(p_node, port::kBoolSettingName);
				if (name && !graph->text(static_cast<uint32_t>(name->i)).empty()) {
					publish_all_ports(p_node, v_bool(true));
				} else if (const std::optional<LogicValue> iname = param(p_node, port::kIntSettingName);
						iname && !graph->text(static_cast<uint32_t>(iname->i)).empty()) {
					publish_all_ports(p_node, v_int(0));
				} else {
					publish_all_ports(p_node, v_float(0.0));
				}
				return;
			}
			case T::Locator:
				if (const std::optional<LogicValue> t = param(p_node, port::kTransform)) {
					publish_all_ports(p_node, *t);
				}
				return;
			default:
				return;
		}
	}

	void auto_start(uint32_t p_node) {
		NodeState &st = nodes[p_node];
		switch (node(p_node).type) {
			case T::Delay:
			case T::RandomDelay:
				if (param_bool(p_node, port::kAutoStart, false)) {
					delay_in(p_node, LogicContext{});
				}
				return;
			case T::Sequence:
			case T::SyncedSequence:
				if (param_bool(p_node, port::kAutoStart, false)) {
					sequence_start(p_node, LogicContext{});
				}
				return;
			case T::BoolToEvent:
				// InitalEvent: the event matching the settled value (false -> OnFalse) [inferred, s4.md §6.2].
				if (param_bool(p_node, port::kInitalEvent, false)) {
					fire(p_node, as_bool(st.value) ? port::kOnTrue : port::kOnFalse, LogicContext{});
				}
				return;
			case T::CompareInt:
			case T::CompareFloat:
				if (param_bool(p_node, port::kTriggerOnStart, false)) {
					compare_fire(p_node, LogicContext{});
				}
				return;
			default:
				return;
		}
	}
};

// --- LogicRuntime --------------------------------------------------------------------------------------------------

LogicRuntime::LogicRuntime(std::unique_ptr<State> p_state) :
		state_(std::move(p_state)) {}
LogicRuntime::LogicRuntime(LogicRuntime &&) noexcept = default;
LogicRuntime &LogicRuntime::operator=(LogicRuntime &&) noexcept = default;
LogicRuntime::~LogicRuntime() = default;

core::Result<LogicRuntime> LogicRuntime::create(std::shared_ptr<const LogicGraph> p_graph, const LogicRuntimeOptions &p_options) {
	if (!p_graph) {
		return core::make_error("logic runtime: no graph");
	}
	if (p_options.hz == 0) {
		return core::make_error("logic runtime: hz must be positive");
	}
	core::Result<LogicGhostLayout> layout = LogicGhostLayout::of(*p_graph);
	if (!layout) {
		return layout.error();
	}
	auto state = std::make_unique<State>();
	state->graph = std::move(p_graph);
	state->options = p_options;
	state->layout = std::move(*layout);
	const size_t n = state->graph->nodes().size();
	state->nodes.resize(n);
	state->inputs.resize(n);
	state->outputs.resize(n);
	state->watch_bits.assign(n, 0);
	return LogicRuntime(std::move(state));
}

core::Status LogicRuntime::watch(uint32_t p_node, int32_t p_port, LogicWatchKind p_kind) {
	State &s = *state_;
	if (s.started) {
		return core::make_error("logic runtime: watch after start");
	}
	if (p_node >= s.nodes.size()) {
		return core::make_error("logic runtime: watch node " + std::to_string(p_node) + " out of range");
	}
	if (s.watches.size() >= s.options.max_watches) {
		return core::make_error("logic runtime: more than " + std::to_string(s.options.max_watches) + " watches");
	}
	s.watches.push_back({ p_node, p_port, p_kind });
	s.watch_bits[p_node] |= p_kind == LogicWatchKind::EventIn ? 1 : 2;
	return core::Status::success();
}

core::Result<LogicTickStats> LogicRuntime::start() {
	State &s = *state_;
	if (s.started) {
		return core::make_error("logic runtime: start() called twice");
	}
	s.started = true;
	const auto t0 = std::chrono::steady_clock::now();
	s.tick = 0;
	s.begin_tick();
	s.init_states();
	s.settling = true;
	for (uint32_t i = 0; i < s.nodes.size() && !s.fault; ++i) {
		s.publish_initial(i);
	}
	const LogicGraph &g = s.g();
	for (uint32_t i = 0; i < s.nodes.size() && !s.fault; ++i) {
		const LogicNode &node = g.nodes()[i];
		for (uint32_t k = 0; k < node.input_count && !s.fault; ++k) {
			const gp::LogicInput &in = g.initial_inputs()[node.input_begin + k];
			Item it;
			it.target = i;
			it.target_port = in.port;
			it.property = true;
			it.value = in.value;
			s.enqueue(it);
		}
	}
	s.drain();
	s.settling = false;
	for (uint32_t i = 0; i < s.nodes.size() && !s.fault; ++i) {
		s.auto_start(i);
	}
	s.drain();
	s.finish_tick();
	s.stats.drain_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
	if (s.fault) {
		return *s.fault;
	}
	return s.stats;
}

core::Status LogicRuntime::force_event(uint32_t p_node, int32_t p_port, LogicContext p_context) {
	State &s = *state_;
	if (p_node >= s.nodes.size()) {
		return core::make_error("logic runtime: forced event node " + std::to_string(p_node) + " out of range");
	}
	if (s.forced.size() >= s.options.max_queue) {
		return core::make_error("logic runtime: more than " + std::to_string(s.options.max_queue) + " forced inputs");
	}
	State::Forced f;
	f.node = p_node;
	f.port = p_port;
	f.context = p_context;
	s.forced.push_back(f);
	return core::Status::success();
}

core::Status LogicRuntime::force_property(uint32_t p_node, int32_t p_port, const LogicValue &p_value) {
	State &s = *state_;
	if (p_node >= s.nodes.size()) {
		return core::make_error("logic runtime: forced property node " + std::to_string(p_node) + " out of range");
	}
	if (s.forced.size() >= s.options.max_queue) {
		return core::make_error("logic runtime: more than " + std::to_string(s.options.max_queue) + " forced inputs");
	}
	State::Forced f;
	f.node = p_node;
	f.port = p_port;
	f.property = true;
	f.value = p_value;
	s.forced.push_back(f);
	return core::Status::success();
}

core::Result<LogicTickStats> LogicRuntime::step() {
	State &s = *state_;
	if (!s.started) {
		return core::make_error("logic runtime: step() before start()");
	}
	if (s.fault) {
		return *s.fault;
	}
	const auto t0 = std::chrono::steady_clock::now();
	if (s.tick == std::numeric_limits<Tick>::max()) {
		return core::make_error("logic runtime: tick counter exhausted");
	}
	++s.tick;
	s.begin_tick();
	s.run_due_timers();
	std::vector<State::Forced> forced;
	forced.swap(s.forced);
	for (const State::Forced &f : forced) {
		if (s.fault) {
			break;
		}
		++s.stats.forced;
		s.record(LogicTraceKind::Forced, f.node, f.port, kNoLogicIndex, 0, f.property ? gp::logic_value_hash(f.value) : 0, f.context.player);
		if (f.property) {
			s.publish(f.node, f.port, f.value);
		} else {
			s.fire(f.node, f.port, f.context);
		}
	}
	s.drain();
	s.finish_tick();
	s.stats.drain_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
	if (s.fault) {
		return *s.fault;
	}
	return s.stats;
}

Tick LogicRuntime::tick() const {
	return state_->tick;
}

const LogicGraph &LogicRuntime::graph() const {
	return *state_->graph;
}

std::span<const LogicEffect> LogicRuntime::effects() const {
	return state_->effects;
}

std::span<const LogicNetworkedEvent> LogicRuntime::networked() const {
	return state_->networked;
}

std::span<const LogicWatchHit> LogicRuntime::watched() const {
	return state_->hits;
}

std::optional<LogicValue> LogicRuntime::output(uint32_t p_node, int32_t p_port) const {
	if (p_node >= state_->outputs.size()) {
		return std::nullopt;
	}
	for (const PortValue &pv : state_->outputs[p_node]) {
		if (pv.port == p_port) {
			return pv.value;
		}
	}
	return std::nullopt;
}

LogicGhost LogicRuntime::ghost() const {
	const State &s = *state_;
	LogicGhost g;
	g.tick = s.tick;
	g.bool_count = s.layout.bools.size();
	g.bool_bits.assign((s.layout.bools.size() + 63) / 64, 0);
	for (size_t i = 0; i < s.layout.bools.size(); ++i) {
		if (as_bool(s.nodes[s.layout.bools[i]].value)) {
			g.bool_bits[i / 64] |= uint64_t(1) << (i % 64);
		}
	}
	for (const uint32_t node : s.layout.transforms) {
		std::array<float, 12> m{};
		if (const std::optional<LogicValue> out = output(node, gp::logic_port::kOut); out && out->kind == K::Transform) {
			m = out->m;
		}
		g.transforms.push_back(m);
	}
	for (const uint32_t node : s.layout.sequences) {
		g.sequence_start.push_back(s.nodes[node].flag ? s.nodes[node].start : LogicGhost::kNoStart);
	}
	return g;
}

const LogicGhostLayout &LogicRuntime::ghost_layout() const {
	return state_->layout;
}

const LogicTrace &LogicRuntime::trace() const {
	return state_->trace;
}

std::span<const LogicUnhandledPort> LogicRuntime::unhandled() const {
	return state_->unhandled;
}

bool LogicRuntime::faulted() const {
	return state_->fault.has_value();
}

const core::Error *LogicRuntime::fault() const {
	return state_->fault ? &*state_->fault : nullptr;
}

} // namespace bf3::sim::logic

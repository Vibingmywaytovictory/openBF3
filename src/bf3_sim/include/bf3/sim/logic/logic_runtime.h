#pragma once
// S4 level logic (docs/design/s4.md §7): the deterministic interpreter over an immutable LogicGraph, its event trace
// and the logic ghost. Godot-free, Jolt-free, single-threaded: one LogicRuntime is owned by the thread that steps
// it (the server thread, plan §2.3, at the logic drain step after rules and entities).
//
// One step() is one simulation tick t:
//   1. timers due at t fire, in (due tick, arm sequence) order;
//   2. inputs forced since the last step (native outputs: a crate destroyed, a flag captured, a player entered)
//      are queued in the order they were forced;
//   3. the FIFO queue is drained: each item is delivered to its target node, whose handler may queue more items
//      (events fan out in edge order; a changed property output queues one item per outgoing property edge, value
//      carried in the item). Drain stops when the queue is empty or after max_deliveries_per_tick deliveries; the
//      rest carries over to t+1 in order (a counted, traced event, never silent);
//   4. effects on native entities (Enabled, Spawn, Destroy, RoundOver, any input into a Native node) recorded during
//      the drain carry apply tick t+1; the native layer applies them before its own update of t+1.
// Delay d seconds arms a timer due at t + logic_delay_ticks(d) (below); 0 ticks delivers in the same drain.
// RandomDelay draws from hash_mix(root_seed, node key, fire count): never hash_keyed, which mixes the per-process
// seed (ids.h).
// start() is tick 0 and returns its LogicTickStats like a step: every node publishes its initial property outputs in
// node order and the constant inputs are queued; the queue drains; then AutoStart nodes (Delay, RandomDelay,
// Sequence) arm in node order (a 0-tick timer fires in this tick) and the queue drains again. The per-tick delivery
// cap counts both drains together; the remainder carries over to tick 1 like any other tick's.
//
// Hostile graphs: the queue holds at most max_queue items and the timer heap at most max_timers; overflow faults
// the runtime (faulted() true, error kept, step() then does nothing and returns the error). Per-tick work is bounded
// by max_deliveries_per_tick plus the handlers' own bounded fan-out (each delivery charges its edges). create() fails
// when the ghost layout does (kMaxGhostEntries).
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::sim::logic {

using core::gameplay::LogicGraph;
using core::gameplay::LogicValue;

// Ticks for a Delay of p_seconds: ceil(p_seconds * hz - 1e-3) in double. The epsilon absorbs float32 storage
// (0.1f * 60 = 6.0000001 would otherwise give 7); 10 s = 600. Not sim_types.h seconds_to_ticks, which rounds to
// nearest (0.105 s: 6 there, 7 here): a logic Delay must never fire before its time, so logic code uses only this.
// Hostile values: NaN, negative and 0 give 0; anything above kMaxLogicDelayTicks gives kMaxLogicDelayTicks.
inline constexpr uint32_t kMaxLogicDelayTicks = 1u << 30;
constexpr uint32_t logic_delay_ticks(double p_seconds, uint32_t p_hz = kSimHz) {
	const double x = p_seconds * double(p_hz) - 1e-3;
	if (!(x > 0.0)) {
		return 0;
	}
	if (x >= double(kMaxLogicDelayTicks)) {
		return kMaxLogicDelayTicks;
	}
	const uint32_t whole = static_cast<uint32_t>(x);
	return double(whole) < x ? whole + 1 : whole;
}

struct LogicRuntimeOptions {
	uint64_t root_seed = 0; // the match seed (plan §2.7); RandomDelay and RandomMultiEvent draw from it
	uint32_t hz = kSimHz;
	size_t max_deliveries_per_tick = 10000;
	size_t max_queue = 65536;
	size_t max_timers = 65536;
	size_t max_effects_per_tick = 65536;
	bool keep_trace_records = false; // the trace hash is always computed; records only when asked (tests, bf3dump)
	size_t max_trace_records = 1u << 20; // records beyond this are hashed and counted, not kept (LogicTrace::truncated)
	size_t max_watches = 1024;
	size_t max_watch_hits_per_tick = 65536; // more faults the runtime
};

// Who caused an event: TeamFilter, PlayerFilter and the native inputs read it. player -1 is none, team 0 is neutral.
struct LogicContext {
	int32_t player = -1;
	uint8_t team = 0;
};

enum class LogicEffectKind : uint8_t {
	Event, // an event delivered into a Native node's input port
	Property, // a property delivered into a Native node's input port (Enabled among them)
};

// An input into a native entity, to apply at apply_tick (the tick after the drain that produced it).
struct LogicEffect {
	Tick apply_tick = 0;
	uint32_t node = 0;
	int32_t port = 0;
	LogicEffectKind kind = LogicEffectKind::Event;
	LogicValue value; // Property
	LogicContext context; // Event
};

// A NetworkedClient / NetworkedClientAndServer event the server fired (plan §3.5: {tick, connection}). `connection`
// is LogicEdge::connection, the EBX connection identity both sides compute from the same data; the client delivers
// it into its own graph's edge with that id (its target may not exist on the server, see
// LogicGraph::networked_edges()). source/source_port are server node indices, for tracing only.
struct LogicNetworkedEvent {
	Tick tick = 0;
	uint64_t connection = 0;
	uint32_t source = 0;
	int32_t source_port = 0;
	LogicContext context;
};

// Watches: the harness (and later the native layer) asks to be told about events at chosen ports without trace
// records. EventIn matches every delivery of an event into (node, port); EventOut every event fired from (node, port),
// counted once per firing, not per edge. Hits are listed per start()/step() in delivery order.
enum class LogicWatchKind : uint8_t { EventIn, EventOut };
struct LogicWatchHit {
	Tick tick = 0;
	uint32_t seq = 0; // the trace sequence number of the delivery within the tick
	uint32_t node = 0;
	int32_t port = 0;
	LogicWatchKind kind = LogicWatchKind::EventIn;
	LogicContext context;
};

enum class LogicTraceKind : uint8_t { Event = 0, Property, TimerFired, Forced, Effect, CarriedOver, Fault };

// One trace record; hashed as a fixed 40-byte little-endian encoding (tick u32, seq u32, kind u8, 3 zero bytes,
// source u32, source port i32, target u32, target port i32, value hash u64, context player i32), so the hash is the
// same on every machine and compiler.
struct LogicTraceRecord {
	Tick tick = 0;
	uint32_t seq = 0; // order within the tick
	LogicTraceKind kind = LogicTraceKind::Event;
	uint32_t source = core::gameplay::kNoLogicIndex;
	int32_t source_port = 0;
	uint32_t target = core::gameplay::kNoLogicIndex;
	int32_t target_port = 0;
	uint64_t value_hash = 0; // FNV-1a 64 of the value's kind and used fields; 0 for events
	int32_t player = -1;
};

struct LogicTrace {
	uint64_t hash = 0xcbf29ce484222325ull; // FNV-1a 64 offset basis; updated per record
	size_t records_total = 0;
	std::vector<LogicTraceRecord> records; // when LogicRuntimeOptions::keep_trace_records, at most max_trace_records
	bool truncated = false; // records_total > records.size() while records were kept
};

struct LogicTickStats {
	Tick tick = 0;
	size_t deliveries = 0;
	size_t timers_fired = 0;
	size_t forced = 0;
	size_t carried_over = 0; // items left in the queue for the next tick
	size_t queue_peak = 0;
	size_t effects = 0;
	size_t networked = 0;
	size_t watch_hits = 0;
	// Wall time of the whole start()/step() (timers, forced inputs, drain, effects), std::chrono::steady_clock
	// (QueryPerformanceCounter on Windows, 100 ns resolution). Noisy on a shared machine; G7 gates percentiles.
	uint64_t drain_ns = 0;
};

// The logic ghost (plan §3.5): the replicated logic state as a plain, serialisable struct. Not networked in S4.
// Layout, each list in (scope path, entity GUID) order: SyncedBool nodes (at most 259 on retail, XP3_Valley
// RushLarge0), SyncedTransform nodes (at most 4 per row), and the sequences whose start tick a late joiner needs:
// every SyncedSequence and every Sequence of realm ClientAndServer (plan §3.5: the start ticks of ClientAndServer
// Sequences gated by synced state; S4 keeps all of them rather than guessing which are gated).
inline constexpr size_t kMaxGhostEntries = 4096; // per list; above it of() fails (encode uses u16 counts and indices)
struct LogicGhostLayout {
	std::vector<uint32_t> bools; // node indices
	std::vector<uint32_t> transforms;
	std::vector<uint32_t> sequences;
	static core::Result<LogicGhostLayout> of(const LogicGraph &p_graph);
};

struct LogicGhost {
	Tick tick = 0;
	size_t bool_count = 0; // = layout.bools.size()
	std::vector<uint64_t> bool_bits; // bit i = bools[i]
	std::vector<std::array<float, 12>> transforms;
	std::vector<Tick> sequence_start; // kNoStart when not playing
	static constexpr Tick kNoStart = 0xffffffffu;

	bool operator==(const LogicGhost &) const = default;
	// Full row (channel 2, on join): tick u32, counts u16 x3, then the payload, little-endian.
	std::vector<uint8_t> encode() const;
	// Fails unless the counts equal p_layout's and the byte length matches exactly.
	static core::Result<LogicGhost> decode(std::span<const uint8_t> p_bytes, const LogicGhostLayout &p_layout);
	// Delta rows (channel 1): (index u16, value) for every bool whose bit differs from p_base.
	struct BoolDelta {
		uint16_t index = 0;
		bool value = false;
	};
	std::vector<BoolDelta> bool_delta(const LogicGhost &p_base) const;
};

// A port of a primitive node that received a delivery its handler does not interpret (a no-op, counted). Review aid:
// bf3dump and the census list them; an empty list on a row means every primitive input there has a behaviour.
struct LogicUnhandledPort {
	core::gameplay::LogicTypeId type = core::gameplay::LogicTypeId::Unknown;
	int32_t port = 0;
	bool event = true; // else a property input
	uint64_t count = 0;
};
inline constexpr size_t kMaxUnhandledPorts = 256;

class LogicRuntime {
public:
	static core::Result<LogicRuntime> create(std::shared_ptr<const LogicGraph> p_graph, const LogicRuntimeOptions &p_options);
	LogicRuntime(LogicRuntime &&) noexcept;
	LogicRuntime &operator=(LogicRuntime &&) noexcept;
	~LogicRuntime();

	// Watches (see LogicWatchKind); before start(). A node out of range or more than max_watches is an error.
	core::Status watch(uint32_t p_node, int32_t p_port, LogicWatchKind p_kind);
	// Tick 0: the init settle and AutoStart (see above). Exactly once, before the first step().
	core::Result<LogicTickStats> start();
	// Inputs from the native layer for the next step: an event leaving (p_node, p_port), or a new value of a native
	// node's property output. Queued in call order. A node index out of range is an error.
	core::Status force_event(uint32_t p_node, int32_t p_port, LogicContext p_context = {});
	core::Status force_property(uint32_t p_node, int32_t p_port, const LogicValue &p_value);
	// Advances one tick. Returns the tick's stats, or the fault.
	core::Result<LogicTickStats> step();

	Tick tick() const; // the last tick stepped (0 after start)
	const LogicGraph &graph() const;
	// Effects, networked events and watch hits produced by the last start() or step().
	std::span<const LogicEffect> effects() const;
	std::span<const LogicNetworkedEvent> networked() const;
	std::span<const LogicWatchHit> watched() const;
	// The current value of a node's property output port; nullopt when the node never published it.
	std::optional<LogicValue> output(uint32_t p_node, int32_t p_port) const;
	LogicGhost ghost() const;
	const LogicGhostLayout &ghost_layout() const;
	const LogicTrace &trace() const;
	// Distinct (type, port, kind) deliveries no handler interpreted, in first-seen order (at most kMaxUnhandledPorts).
	std::span<const LogicUnhandledPort> unhandled() const;
	bool faulted() const;
	const core::Error *fault() const;

private:
	struct State;
	explicit LogicRuntime(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

} // namespace bf3::sim::logic

#pragma once

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

enum class LogicPlatform : uint8_t { Win32 = 0, Xenon = 1, Ps3 = 2 };

struct LogicRuntimeOptions {
	uint64_t root_seed = 0;
	uint32_t hz = kSimHz;
	size_t max_deliveries_per_tick = 10000;
	size_t max_queue = 65536;
	size_t max_timers = 65536;
	size_t max_effects_per_tick = 65536;
	bool keep_trace_records = false;
	size_t max_trace_records = 1u << 20;
	size_t max_watches = 1024;
	size_t max_watch_hits_per_tick = 65536;
	LogicPlatform platform = LogicPlatform::Win32;
};

struct LogicContext {
	int32_t player = -1;
	uint8_t team = 0;
};

enum class LogicEffectKind : uint8_t {
	Event,
	Property,
};

struct LogicEffect {
	Tick apply_tick = 0;
	uint32_t node = 0;
	int32_t port = 0;
	LogicEffectKind kind = LogicEffectKind::Event;
	LogicValue value;
	LogicContext context;
};

struct LogicNetworkedEvent {
	Tick tick = 0;
	uint64_t connection = 0;
	uint32_t source = 0;
	int32_t source_port = 0;
	LogicContext context;
};

enum class LogicWatchKind : uint8_t { EventIn, EventOut };
struct LogicWatchHit {
	Tick tick = 0;
	uint32_t seq = 0;
	uint32_t node = 0;
	int32_t port = 0;
	LogicWatchKind kind = LogicWatchKind::EventIn;
	LogicContext context;
};

enum class LogicTraceKind : uint8_t { Event = 0, Property, TimerFired, Forced, Effect, CarriedOver, Fault };

struct LogicTraceRecord {
	Tick tick = 0;
	uint32_t seq = 0;
	LogicTraceKind kind = LogicTraceKind::Event;
	uint32_t source = core::gameplay::kNoLogicIndex;
	int32_t source_port = 0;
	uint32_t target = core::gameplay::kNoLogicIndex;
	int32_t target_port = 0;
	uint64_t value_hash = 0;
	int32_t player = -1;
};

struct LogicTrace {
	uint64_t hash = 0xcbf29ce484222325ull;
	size_t records_total = 0;
	std::vector<LogicTraceRecord> records;
	bool truncated = false;
};

struct LogicTickStats {
	Tick tick = 0;
	size_t deliveries = 0;
	size_t timers_fired = 0;
	size_t forced = 0;
	size_t carried_over = 0;
	size_t queue_peak = 0;
	size_t effects = 0;
	size_t networked = 0;
	size_t watch_hits = 0;

	uint64_t drain_ns = 0;
};

inline constexpr size_t kMaxGhostEntries = 4096;
struct LogicGhostLayout {
	std::vector<uint32_t> bools;
	std::vector<uint32_t> transforms;
	std::vector<uint32_t> sequences;
	static core::Result<LogicGhostLayout> of(const LogicGraph &p_graph);
};

struct LogicGhost {
	Tick tick = 0;
	size_t bool_count = 0;
	std::vector<uint64_t> bool_bits;
	std::vector<std::array<float, 12>> transforms;
	std::vector<Tick> sequence_start;
	static constexpr Tick kNoStart = 0xffffffffu;

	bool operator==(const LogicGhost &) const = default;

	std::vector<uint8_t> encode() const;

	static core::Result<LogicGhost> decode(std::span<const uint8_t> p_bytes, const LogicGhostLayout &p_layout);

	struct BoolDelta {
		uint16_t index = 0;
		bool value = false;
	};
	std::vector<BoolDelta> bool_delta(const LogicGhost &p_base) const;
};

struct LogicUnhandledPort {
	core::gameplay::LogicTypeId type = core::gameplay::LogicTypeId::Unknown;
	int32_t port = 0;
	bool event = true;
	uint64_t count = 0;
};
inline constexpr size_t kMaxUnhandledPorts = 256;

class LogicRuntime {
public:
	static core::Result<LogicRuntime> create(std::shared_ptr<const LogicGraph> p_graph, const LogicRuntimeOptions &p_options);
	LogicRuntime(LogicRuntime &&) noexcept;
	LogicRuntime &operator=(LogicRuntime &&) noexcept;
	~LogicRuntime();

	core::Status watch(uint32_t p_node, int32_t p_port, LogicWatchKind p_kind);

	core::Result<LogicTickStats> start();

	core::Status force_event(uint32_t p_node, int32_t p_port, LogicContext p_context = {});
	core::Status force_property(uint32_t p_node, int32_t p_port, const LogicValue &p_value);
	core::Status deliver_event(uint32_t p_node, int32_t p_port, LogicContext p_context = {});

	core::Result<LogicTickStats> step();

	Tick tick() const;
	const LogicGraph &graph() const;

	std::span<const LogicEffect> effects() const;
	std::span<const LogicNetworkedEvent> networked() const;
	std::span<const LogicWatchHit> watched() const;

	std::optional<LogicValue> output(uint32_t p_node, int32_t p_port) const;
	LogicGhost ghost() const;
	const LogicGhostLayout &ghost_layout() const;
	const LogicTrace &trace() const;

	std::span<const LogicUnhandledPort> unhandled() const;
	bool faulted() const;
	const core::Error *fault() const;

private:
	struct State;
	explicit LogicRuntime(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

}

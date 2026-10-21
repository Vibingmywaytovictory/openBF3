#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::sim::logic {

inline constexpr uint32_t kMcomSearchHops = 4;
inline constexpr uint32_t kParkSearchHops = 4;

inline constexpr uint32_t kConquestGateHops = 4;

enum class LogicScriptKind : uint8_t {
	Idle,
	RushMcoms,
	ConquestCaptures,
};

inline constexpr Tick kMaxLogicScriptTicks = Tick(1) << 22;

struct LogicScriptOptions {
	LogicScriptKind kind = LogicScriptKind::Idle;
	Tick ticks = 60 * 60 * 10;
	Tick rush_destroy_after = 60 * 60;

	Tick round_start_tick = 1;

	struct Capture {
		Tick tick = 0;
		core::gameplay::LogicNodeRef flag;
		uint8_t team = 1;
	};
	std::vector<Capture> conquest_order;

	struct Force {
		Tick tick = 0;
		uint32_t node = 0;
		int32_t port = 0;
	};
	std::vector<Force> forces;
	LogicRuntimeOptions runtime;
};

LogicScriptOptions rush_mcoms_script();

LogicScriptOptions caspian_conquest_script();

struct LogicRunReport {
	Tick ticks = 0;
	uint64_t trace_hash = 0;
	size_t trace_records = 0;
	bool trace_truncated = false;
	size_t deliveries = 0;
	size_t max_deliveries_tick = 0;
	size_t carried_over = 0;
	size_t effects = 0;
	size_t networked = 0;
	LogicTickStats tick0;

	uint64_t drain_ns_p50 = 0, drain_ns_p99 = 0, drain_ns_max = 0;
	uint64_t drain_ns_busy_p99 = 0;
	size_t busy_ticks = 0;
	bool faulted = false;
	std::string fault;
	std::vector<NativeTimelineEntry> timeline;
	std::vector<LogicWatchHit> watch_hits;
	std::vector<LogicTraceRecord> trace;

	struct Action {
		Tick tick = 0;
		uint32_t node = 0;
		enum class What : uint8_t { PlayerEnter, Destroy, Capture, RoundStart } what = What::Destroy;
		uint8_t team = 0;
	};
	std::vector<Action> actions;
	std::vector<LogicUnhandledPort> unhandled;
	LogicGhost ghost;
};

core::Result<LogicRunReport> run_logic_script(std::shared_ptr<const core::gameplay::LogicGraph> p_graph, const LogicScriptOptions &p_options);

struct LogicGateVerdict {
	bool pass = false;
	std::string detail;
};

LogicGateVerdict judge_rush_spawner_order(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);

LogicGateVerdict judge_park_delay(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);

struct ConquestSelection {
	std::vector<uint32_t> gated;
	std::vector<uint32_t> constant;
	std::vector<uint32_t> flags;
};
ConquestSelection select_conquest_spawners(const core::gameplay::LogicGraph &p_graph);

struct ConquestExpectation {
	core::gameplay::LogicNodeRef spawner;
	bool gated = true;
	const char *label = "";
	bool enabled[5] = {};
};
inline constexpr Tick kConquestCheckTicks[5] = { 1, 601, 1201, 1801, 2401 };

std::span<const ConquestExpectation> caspian_conquest_expected();

LogicGateVerdict judge_conquest_timeline(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report,
		std::span<const ConquestExpectation> p_expected);

struct LogicCensusRow {
	std::string level, mode;
	bool ok = false;
	std::string error;
	core::gameplay::LogicGraphStats stats;
	size_t nodes = 0;
	size_t ghost_bools = 0, ghost_transforms = 0, ghost_sequences = 0;
	double build_ms = 0;
	LogicTickStats tick0;
	size_t idle_deliveries = 0;
	uint64_t idle_p99_ns = 0;
	size_t unhandled = 0;
};
inline constexpr Tick kCensusIdleTicks = 600;
LogicCensusRow logic_census_row(std::string p_level, std::string p_mode, std::shared_ptr<const core::gameplay::LogicGraph> p_graph, double p_build_ms);
std::string logic_census_header();
std::string logic_census_line(const LogicCensusRow &p_row);

}

#pragma once
// S4 level logic (docs/design/s4.md §9): scripted runs of a level's logic graph and the gate judges. The ctests
// (logic.metro, logic.caspian, logic.census) and `bf3dump logic run|trace|census` call the same functions, so a gate
// measured by ctest and by bf3dump is the same computation.
//
// A script is built from the graph's structure, never from GUIDs typed into code: the MCOM objects are the
// StaticModel nodes whose OnDestroyed reaches a MultipleTrigger with TriggerAmount 2 within kMcomSearchHops event
// hops (that MultipleTrigger is the base), the Rush spawners are the CharacterSpawn nodes whose Enabled input is fed
// by a SyncedBool, the Conquest flags are the CapturePoint nodes. Scripts are adaptive where the data is: a base's
// MCOMs are destroyed a fixed delay after the base's activation reaches them in this run.
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::sim::logic {

inline constexpr uint32_t kMcomSearchHops = 4;

enum class LogicScriptKind : uint8_t {
	Idle, // start and run, no actions
	RushMcoms, // a player of each team enters at tick 1; each base's MCOMs destroyed rush_destroy_after ticks after activation
	ConquestCaptures, // flags captured at fixed ticks in conquest_order (team, flag ordinal by node order)
};

struct LogicScriptOptions {
	LogicScriptKind kind = LogicScriptKind::Idle;
	Tick ticks = 60 * 60 * 10; // run length (10 min at 60 Hz)
	Tick rush_destroy_after = 60 * 60; // 60 s after a base's MCOMs are activated
	struct Capture {
		Tick tick = 0;
		uint32_t flag = 0; // ordinal among the graph's CapturePoint nodes, node order
		uint8_t team = 1;
	};
	std::vector<Capture> conquest_order;
	LogicRuntimeOptions runtime;
};

struct LogicRunReport {
	Tick ticks = 0;
	uint64_t trace_hash = 0;
	size_t trace_records = 0;
	size_t deliveries = 0;
	size_t max_deliveries_tick = 0;
	size_t carried_over = 0;
	size_t effects = 0;
	size_t networked = 0;
	uint64_t drain_ns_p50 = 0, drain_ns_p99 = 0, drain_ns_max = 0; // noisy (shared machine)
	bool faulted = false;
	std::string fault;
	std::vector<NativeTimelineEntry> timeline; // the stubs' state changes
	std::vector<LogicTraceRecord> trace; // when runtime.keep_trace_records
	// What the script did: (tick, node, action) for every forced input.
	struct Action {
		Tick tick = 0;
		uint32_t node = 0;
		enum class What : uint8_t { PlayerEnter, Destroy, Capture } what = What::Destroy;
		uint8_t team = 0;
	};
	std::vector<Action> actions;
};

// Creates the runtime and the stubs, starts, runs p_options.ticks steps with the script's actions.
core::Result<LogicRunReport> run_logic_script(std::shared_ptr<const core::gameplay::LogicGraph> p_graph, const LogicScriptOptions &p_options);

struct LogicGateVerdict {
	bool pass = false;
	std::string detail; // one line per measured value, for the ctest log and bf3dump
};

// G2: the Rush spawners open in base order (docs/design/s4.md §1).
LogicGateVerdict judge_rush_spawner_order(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);
// G3: the park SyncedBool turns true exactly ticks(10 s) after base 1's fall event reaches the park schematic gate.
LogicGateVerdict judge_park_delay(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);
// G5: the Conquest vehicle and flag spawner Enabled timeline under the scripted captures.
LogicGateVerdict judge_conquest_timeline(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);

} // namespace bf3::sim::logic

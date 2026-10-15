#pragma once
// S4 level logic (docs/design/s4.md §9): scripted runs of a level's logic graph and the gate judges. The ctests
// (logic.metro, logic.caspian, logic.perf, logic.census) and `bf3dump logic run|trace|census` call the same
// functions, so a gate measured by ctest and by bf3dump is the same computation.
//
// Targets are selected from the graph's structure (after collapse, so interface hops are already gone):
//  * Rush (§9.1): the MCOMs are the StaticModel nodes whose OnDestroyed reaches a MultipleTrigger with TriggerAmount 2
//    within kMcomSearchHops event hops (that MultipleTrigger is the base); an MCOM's activation is the Enable event
//    into the first DelayTrigger on that path (watched, LogicWatchKind::EventIn); the Rush spawners are the
//    CharacterSpawn nodes whose Enabled input is fed by a SyncedBool; the park SyncedBool is the one reached from base
//    1's OnTrigger within kParkSearchHops event hops through exactly one Delay, whose Delay param is 10 s.
//  * Conquest (§9.2): see select_conquest_spawners.
// The interpreter never uses GUIDs. Scripts and judges use them only as test data, to name the flags a script
// captures and to cross-check the structural selections against the GUIDs documented in s4.md §13 (G2 and G3 print
// the cross-check; G6 requires it, because its timeline is exact per spawner).
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
// Backward property hops from a spawner's Enabled through Bool/SyncedBool/Not/And/Or to the gating Bool/SyncedBool.
inline constexpr uint32_t kConquestGateHops = 4;

enum class LogicScriptKind : uint8_t {
	Idle, // start and run, no actions
	RushMcoms, // a player of each team enters at tick 1; each MCOM destroyed rush_destroy_after ticks after activation
	ConquestCaptures, // the captures of conquest_order at their ticks
};

struct LogicScriptOptions {
	LogicScriptKind kind = LogicScriptKind::Idle;
	Tick ticks = 60 * 60 * 10; // ticks stepped after start() (10 min at 60 Hz)
	Tick rush_destroy_after = 60 * 60; // 60 s after an MCOM's activation
	// One capture: at `tick` (timing convention of LogicNativeStubs: called after apply(tick), before step(tick)),
	// the CapturePoint found by `flag` (LogicGraph::find_nodes with type CapturePoint; exactly one node, else the run
	// fails) is captured by `team`.
	struct Capture {
		Tick tick = 0;
		core::gameplay::LogicNodeRef flag;
		uint8_t team = 1;
	};
	std::vector<Capture> conquest_order;
	LogicRuntimeOptions runtime;
};

// The scripts of the gates, as s4.md §9 defines them (shared by the ctests and bf3dump).
LogicScriptOptions rush_mcoms_script(); // G2-G4: 36,000 ticks, destroy 3,600 ticks after activation
// G6: Team1 captures f3042a9e at 600, Team2 e4c5d622 at 1,200, Team2 f3042a9e at 1,800, Team1 e4c5d622 at 2,400;
// 3,000 ticks.
LogicScriptOptions caspian_conquest_script();

struct LogicRunReport {
	Tick ticks = 0;
	uint64_t trace_hash = 0;
	size_t trace_records = 0; // records_total
	bool trace_truncated = false;
	size_t deliveries = 0;
	size_t max_deliveries_tick = 0; // over all ticks including tick 0
	size_t carried_over = 0; // sum over all ticks including tick 0
	size_t effects = 0;
	size_t networked = 0;
	LogicTickStats tick0; // start()
	// Wall clock (steady_clock), noisy on a shared machine. "busy" = ticks with deliveries > 0. Warm-up is included;
	// tick 0 is in max only.
	uint64_t drain_ns_p50 = 0, drain_ns_p99 = 0, drain_ns_max = 0;
	uint64_t drain_ns_busy_p99 = 0;
	size_t busy_ticks = 0;
	bool faulted = false;
	std::string fault;
	std::vector<NativeTimelineEntry> timeline; // the stubs' state changes
	std::vector<LogicWatchHit> watch_hits; // every hit of the script's watches, in order
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

// Creates the runtime and the stubs, sets the script's watches, starts, runs p_options.ticks steps with the script's
// actions. The judges read watch_hits and timeline only, so trace truncation cannot change a verdict.
core::Result<LogicRunReport> run_logic_script(std::shared_ptr<const core::gameplay::LogicGraph> p_graph, const LogicScriptOptions &p_options);

struct LogicGateVerdict {
	bool pass = false;
	std::string detail; // one line per measured value, for the ctest log and bf3dump
};

// G2: the Rush spawners open in base order (docs/design/s4.md §1).
LogicGateVerdict judge_rush_spawner_order(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);
// G3: the park SyncedBool's SetTrue arrives exactly logic_delay_ticks(10 s) = 600 ticks after base 1's OnTrigger.
LogicGateVerdict judge_park_delay(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report);

// G6 structural selection on a Conquest graph (s4.md §9.2):
//  gated: VehicleSpawn nodes whose Enabled property input reaches, within kConquestGateHops backward property hops
//         through Bool/SyncedBool/Not/And/Or, a Bool or SyncedBool with a SetTrue or SetFalse event input from a
//         TeamFilter whose In is fed by a CapturePoint's OnCaptured;
//  constant: VehicleSpawn nodes whose Enabled input is fed by a SyncedBool that has no event input and no property
//         input (its In is a constant);
//  flags: the CapturePoint nodes that feed a gated chain.
// Each list in node order.
struct ConquestSelection {
	std::vector<uint32_t> gated;
	std::vector<uint32_t> constant;
	std::vector<uint32_t> flags;
};
ConquestSelection select_conquest_spawners(const core::gameplay::LogicGraph &p_graph);

// One spawner of the G6 table: its identity and its Enabled value after the effects of each listed apply tick.
struct ConquestExpectation {
	core::gameplay::LogicNodeRef spawner; // resolves to exactly one VehicleSpawn node
	bool gated = true; // must be in select_conquest_spawners().gated (else .constant)
	const char *label = ""; // "M1A2", "jet f18 5b7bb147", ...
	bool enabled[5] = {}; // at ticks 1, 601, 1,201, 1,801, 2,401 (s4.md §9.2)
};
inline constexpr Tick kConquestCheckTicks[5] = { 1, 601, 1201, 1801, 2401 };
// The 10 rows of s4.md §9.2 (6 gated, 4 constant jets).
std::span<const ConquestExpectation> caspian_conquest_expected();

// G6: the selection equals the expectation rows (gated and constant sets, by identity); every row's Enabled state
// after apply(t) equals enabled[] at each t of kConquestCheckTicks; and the timeline holds no other Enabled change on
// those 10 spawners in ticks 1..3,000 (changes at tick 1 from the stub's initial own-field value are the initial
// values, checked by enabled[0]).
LogicGateVerdict judge_conquest_timeline(const core::gameplay::LogicGraph &p_graph, const LogicRunReport &p_report,
		std::span<const ConquestExpectation> p_expected);

} // namespace bf3::sim::logic

#pragma once
// S4 level logic (docs/design/s4.md §8): the native side of the graph. Gameplay entities (spawners, capture points,
// MCOM objects, combat areas, counters, round flow, area triggers) stay C++ (plan D12 revised); the interpreter only
// sees their ports, bound by id. In S4 they are stubs: they keep the state the gates read (enabled, owner team,
// destroyed, tickets), apply the runtime's effects at their apply tick, and turn harness actions (capture, destroy,
// player enter) into the outputs the real entity would fire. M2b/M4/M6 replace the stub bodies with the real
// systems behind the same bindings.
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bf3::sim::logic {

using core::gameplay::LogicTypeId;

enum class NativePortDir : uint8_t { EventIn, EventOut, PropertyIn, PropertyOut, LinkIn, LinkOut };

// What a bound port means to the stub. Ports the data uses that S4 does not interpret bind as Recorded: the effect
// is kept in the timeline and otherwise ignored.
enum class NativePortMeaning : uint8_t {
	Recorded = 0,
	Enabled, // PropertyIn bool: the entity is active (spawners, triggers, interactions, counters)
	Enable, // EventIn
	Disable, // EventIn
	Spawn, // EventIn: vehicle spawner spawns now
	Destroy, // EventIn
	OnDestroyed, // EventOut
	OnSpawned, // EventOut
	OnCaptured, // EventOut, context team = new owner
	OnLost, // EventOut, context team = previous owner
	OwnerTeam, // PropertyOut int
	LinkedSpawner, // LinkOut: a capture point's own spawners (enabled for the owner team natively)
	OnPlayerEnter, // EventOut (HumanPlayer: a player joined; area triggers: entered)
	RoundStarted, // EventIn (RoundOver) / EventOut (PreRound)
	OnRoundOver, // EventOut
	Activate, // EventIn: an MCOM's interaction armed by the base chain
};

// One known port of a native type. `name` is empty for the ports whose name is not recoverable from the hash.
struct NativePortBinding {
	LogicTypeId type = LogicTypeId::Unknown;
	NativePortDir dir = NativePortDir::EventIn;
	int32_t port = 0;
	NativePortMeaning meaning = NativePortMeaning::Recorded;
	std::string_view name;
};

// The bindings of one type (empty for non-native types), in port id order.
std::span<const NativePortBinding> native_ports(LogicTypeId p_type);
std::optional<NativePortBinding> native_port(LogicTypeId p_type, NativePortDir p_dir, int32_t p_port);

// One change of a stub's state, in apply order. The Enabled timelines of the Rush and Conquest gates are this list
// filtered by node and kind.
struct NativeTimelineEntry {
	Tick tick = 0;
	uint32_t node = 0;
	enum class What : uint8_t { Enabled, Destroyed, Owner, Spawned, RoundOver, Input } what = What::Input;
	int64_t value = 0; // Enabled 0/1, Owner team, Input port id
};

struct NativeStubState {
	bool enabled = false;
	bool destroyed = false;
	uint8_t owner = 0; // capture points
	int64_t tickets = 0; // ticket and life counters
};

// The stubs of every Native node of a graph. Single-threaded, owned next to the LogicRuntime it feeds.
class LogicNativeStubs {
public:
	static core::Result<LogicNativeStubs> create(std::shared_ptr<const core::gameplay::LogicGraph> p_graph);
	LogicNativeStubs(LogicNativeStubs &&) noexcept;
	LogicNativeStubs &operator=(LogicNativeStubs &&) noexcept;
	~LogicNativeStubs();

	// Applies the runtime's effects whose apply tick is p_tick (call before the runtime's step for p_tick), and
	// forces the outputs those changes cause (a capture point's linked spawners, a spawner's OnSpawned).
	core::Status apply(Tick p_tick, std::span<const LogicEffect> p_effects, LogicRuntime &r_runtime);
	// Harness actions: the real entity's outputs, forced into the runtime for its next step.
	core::Status player_enter(uint32_t p_human_player, LogicContext p_player, LogicRuntime &r_runtime);
	core::Status destroy(uint32_t p_node, LogicRuntime &r_runtime);
	core::Status capture(uint32_t p_capture_point, uint8_t p_team, LogicRuntime &r_runtime);

	std::optional<NativeStubState> state(uint32_t p_node) const;
	std::span<const NativeTimelineEntry> timeline() const;

private:
	struct State;
	explicit LogicNativeStubs(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

} // namespace bf3::sim::logic

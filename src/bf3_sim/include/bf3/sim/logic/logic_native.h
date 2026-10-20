#pragma once

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

namespace native_id {

inline constexpr int32_t kSpawnerUnknownOut = 154856772;

inline constexpr int32_t kCaptureCharacterSpawnLink = -320184006;

inline constexpr int32_t kCaptureVehicleSpawnLink = -275573010;
}

enum class NativePortDir : uint8_t { EventIn, EventOut, PropertyIn, PropertyOut, LinkIn, LinkOut };

enum class NativePortMeaning : uint8_t {
	Recorded = 0,
	Enabled,
	Enable,
	Disable,
	Spawn,
	Destroy,
	OnDestroyed,
	OnSpawned,
	OnCaptured,
	OnLost,
	OwnerTeam,
	LinkedSpawner,
	OnPlayerEnter,
	RoundStarted,
	OnRoundOver,
	Activate,

	Kills,
	KillLimit,
	TeamWins,
	StopTime,
	UnSpawnAllSoldiers,
	OnPlayerSpawned,
	OnPlayerDied,
	OnSpectatorEnter,
	PlayerEnter,
	DisableAllowedToSpawnOn,
	Deactivate,
	RestartLevel,
	StartRound,
	EnablePlayerMovement,
};

struct NativePortBinding {
	LogicTypeId type = LogicTypeId::Unknown;
	NativePortDir dir = NativePortDir::EventIn;
	int32_t port = 0;
	NativePortMeaning meaning = NativePortMeaning::Recorded;
	std::string_view name;
};

std::span<const NativePortBinding> native_ports(LogicTypeId p_type);
std::optional<NativePortBinding> native_port(LogicTypeId p_type, NativePortDir p_dir, int32_t p_port);

inline constexpr size_t kMaxNativeTimelineEntries = size_t(1) << 22;
struct NativeTimelineEntry {
	Tick tick = 0;
	uint32_t node = 0;
	enum class What : uint8_t { Enabled, Destroyed, Owner, Spawned, RoundOver, Input } what = What::Input;
	int64_t value = 0;
};

struct NativeStubState {
	bool enabled = false;
	bool destroyed = false;
	uint8_t owner = 0;
	int64_t tickets = 0;
};

class LogicNativeStubs {
public:
	static core::Result<LogicNativeStubs> create(std::shared_ptr<const core::gameplay::LogicGraph> p_graph);
	LogicNativeStubs(LogicNativeStubs &&) noexcept;
	LogicNativeStubs &operator=(LogicNativeStubs &&) noexcept;
	~LogicNativeStubs();

	core::Status apply(Tick p_tick, std::span<const LogicEffect> p_effects, LogicRuntime &r_runtime);

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

}

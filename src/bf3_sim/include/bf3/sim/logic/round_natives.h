#pragma once
// The TDM native entities behind S4's bindings (docs/design/m2b.md §3.1, §2.6 #4; declared in W1.0, owner ru).
// Godot-free, single-threaded, owned beside the LogicRuntime it feeds (on the server thread by host::TdmRules), the way
// LogicNativeStubs is (logic_native.h). apply(tick, effects, runtime) runs before each runtime step. Types it does not
// implement (FriendZone, CombatAreaTrigger, Setting, TicketCounter and every Conquest / Rush type) it hands to an
// embedded LogicNativeStubs, where they stay Recorded, so one object covers the whole graph and M4 adds the Conquest
// natives without changing an S4 or M2b row.
//
// What each native does is m2b.md §3.1's table: KillCounter (+EnemyWeight per enemy kill by its team, team kills and
// suicides 0, bots count; Kills on change, KillLimit once), PreRound (the player count, at >= RoundMinPlayerCount a
// RoundRestartCountdown countdown, then StartRound and EnablePlayerMovement in that tick; preround_override_s 0 skips
// it), RoundOver (RoundStarted arms, TeamWins sets the winner and fires OnRoundOver, the lower node index on a tie),
// HumanPlayer (OnPlayerEnter / OnPlayerSpawned / OnPlayerDied / OnSpectatorEnter with the player's LogicContext;
// UnSpawnAllSoldiers -> take_unspawn_all), AutoTeam (the smaller team, team 1 on a tie; balance moves bots, never a
// human while bots can; RotateTeamOnNewRound swaps at the restart), InputRestriction (Activate / Deactivate, the
// allowed-bit mask from its fields as net::restrict_bits), LevelControl (RestartLevel and the post-round output ->
// take_restart), CharacterSpawn / AlternateSpawn (the point sets per team, Enabled tracked). Every timing is a named
// field of config/tdm_tuning.cfg read once at load (ru).
//
// The E1 coverage check (§3.1): LogicRuntime::unhandled() lists primitive ports only, and a native port without a
// real handler lands in the stubs as Recorded, so E1 reads coverage(): after a round every TDM type of the table has
// recorded == 0; Recorded is allowed only for FriendZone, CombatAreaTrigger and Setting.
//
// Determinism (§9.2): the runtime draws from LogicRuntimeOptions::root_seed = the match seed; the natives iterate
// slots and nodes in index order; no wall clock. Hostile data (a graph from the install) fails create() or a call
// with an error, never a crash; every per-tick list is bounded (kMaxNativeTimelineEntries).

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <span>

namespace bf3::sim::logic {

struct RoundNativeConfig {
	// Metro TDM's cap; 64 for E8's 64-bot run (AutoTeam's TamFullTeams would leave half the bots out otherwise).
	uint8_t max_players = 32;
	int kill_limit_override = 0; // > 0 replaces KillCounter.MaxKillCount (--kill-limit; dev, not in the digest, Q12)
	float preround_override_s = -1.0f; // < 0: RoundRestartCountdown (10 s); 0: no countdown (tests, --preround=0)
};

enum class RoundPhase : uint8_t { PreRound = 0, InRound = 1, RoundOver = 2, Restarting = 3 };

class RoundNatives {
public:
	static core::Result<RoundNatives> create(std::shared_ptr<const core::gameplay::LogicGraph> p_graph, const RoundNativeConfig &p_config);
	RoundNatives(RoundNatives &&) noexcept;
	RoundNatives &operator=(RoundNatives &&) noexcept;
	~RoundNatives();

	// The runtime's effects whose apply tick is p_tick (before the runtime's step for p_tick), as LogicNativeStubs.
	core::Status apply(Tick p_tick, std::span<const LogicEffect> p_effects, LogicRuntime &r_runtime);

	// Game -> graph: each forces the native's outputs into the runtime's next step (the stubs' timing convention).
	core::Status player_enter(uint8_t p_slot, bool p_bot, LogicRuntime &r_runtime); // HumanPlayer.OnPlayerEnter -> AutoTeam
	core::Status player_leave(uint8_t p_slot, LogicRuntime &r_runtime);
	core::Status player_spawned(uint8_t p_slot, uint8_t p_team, LogicRuntime &r_runtime);
	// p_killer -1 (or == p_slot) for a fall or a suicide (counts 0).
	core::Status player_died(uint8_t p_slot, uint8_t p_team, int p_killer, uint8_t p_killer_team, LogicRuntime &r_runtime);
	core::Status kill_undone(uint8_t p_killer_team, LogicRuntime &r_runtime); // a revive takes the kill back (§3.6, J4)
	core::Status tick_round(Tick p_tick, LogicRuntime &r_runtime); // the PreRound countdown and the player counts

	// Graph -> game.
	RoundPhase phase() const;
	uint8_t winner() const; // 0 none, 1, 2
	int kills(uint8_t p_team) const; // KillCounter.Kills of team 1 / 2 (0 for any other)
	int kill_limit() const;
	bool input_restricted() const;
	uint32_t restriction_mask() const; // net::restrict_bits:: allowed while restricted
	Tick countdown_end_tick() const; // the PreRound countdown's StartRound tick (0 = not counting down)
	uint8_t team_of(uint8_t p_slot) const; // AutoTeam's choice (0 = not in the game)
	bool take_unspawn_all(); // edge-triggered: HumanPlayer.UnSpawnAllSoldiers since the last take
	bool take_restart(); // edge-triggered: LevelControl.RestartLevel / the post-round output since the last take
	float player_killed_delay_s() const; // HumanPlayer.PlayerKilledDelay (5.0 on Metro TDM)
	bool killer_camera() const; // the Setting "BFServer.IsKillerCameraEnabled" (default true)
	std::span<const NativeTimelineEntry> timeline() const;

	// Per native type: deliveries this object handled vs. handed to the embedded stubs as Recorded (E1). One row per
	// native type present in the graph, ascending LogicTypeId.
	struct NativeCoverage {
		core::gameplay::LogicTypeId type = core::gameplay::LogicTypeId::Unknown;
		uint32_t handled = 0;
		uint32_t recorded = 0;
	};
	std::span<const NativeCoverage> coverage() const;

private:
	struct State;
	explicit RoundNatives(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

} // namespace bf3::sim::logic

#pragma once
// The TDM rules on the server thread (docs/design/m2b.md §3.2-§3.6, §2.6 #1, #2; declared in W1.0, owner ru). One
// TdmRules per GameServer when GameServerConfig::rules.mode == Tdm: it owns the LogicRuntime over SharedLevel::logic
// with its RoundNatives (round_natives.h), and is the server's RespawnPolicy and a CombatEventSink. GameServer reaches
// it only through the named insertions of §3.2 (game_server.{h,cpp}, each line marked "M2b ru [tag]"): [cfg] [sv]
// [init] [tdm bots] [add] [cmd] [rules] [orders] [recv] [snap] [view] and R1-R3, R7, R8, R10. Without rules every M2a
// path is unchanged.
//
// Per tick (§3.2 [rules], inside sv_rules after apply_rules and before the copy-back): the logic drain and the natives,
// regen (writes r_states[slot].health, the only writable copy), man-down expiry, the defib test over
// Gunplay::interval_shots() (a per-slot shot_index cursor, so each shot is seen once), the round flow; what the
// GameServer must do comes back as RulesOrders, executed by Impl::sv_orders in [orders] in vector order.
//
// Slots >= net::kMaxPlayerRows (64, lag_comp's kMaxLagSlots) are spectators with rules on: never filled by
// add_tdm_bots, refused by request_spawn (Spectator, counted), never ordered to spawn (§3.2).
//
// Hostile clients (§9.3): request_spawn validates everything and counts each refusal (RulesStats); the defib test is
// the server's own (downed list, corpse positions), so a client cannot revive an enemy, a living soldier or an
// expired corpse; names are cut to net::kMaxPlayerNameBytes of valid UTF-8 for the rows.

#include "bf3/core/result.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/logic/round_natives.h" // M2b ru
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string> // M2b ru
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::host {

struct BotLevel; // bot_fill.h

// M2b ru (as built, docs/design/m2b-ru.md §3): config/tdm_tuning.cfg, the TDM numbers the BF3 data does not decide.
// Every key is a named default [inferred, measured = false] unless its comment says [verified]; the loader reads it
// once with SharedLoadOptions::tdm (SharedLoadOptions::tdm_tuning_cfg; empty = these defaults) and fills the scoring
// rows from the data (read_scoring), so SharedLevel::tdm_tuning is what both ends use. Format: "key = value" lines,
// '#' comments; an unknown key, a bad number or a file above kMaxTdmTuningBytes is an error.
inline constexpr std::string_view kTdmTuningPath = "config/tdm_tuning.cfg";
inline constexpr size_t kMaxTdmTuningBytes = 64 * 1024;
inline constexpr size_t kMaxImmunityAbortEntries = 64;
struct ImmunityAbortEntry {
	int32_t value = 0; // a raw EntryInputActionEnum value of SoldierDef::abort_spawn_immortality_inputs
	uint32_t buttons = 0; // sim::buttons:: it maps to
	bool move = false; // or the two movement axes
};
struct TdmTuning {
	// Respawn (m2b.md §3.4).
	float revive_wait_radius_m = 20.0f; // a bot with a living assault teammate this close to its corpse waits ...
	float revive_wait_s = 8.0f; // ... this long (inside the 13 s man-down) instead of PlayerKilledDelay
	float restart_estimate_s = 60.0f; // MatchState::restart_tick after UnSpawnAllSoldiers (the graph's Delay 60 s)
	// Revive (§3.6).
	bool revive_takes_kill = true; // the killer team's KillCounter -1 (Q2)
	float defib_range_m = 1.8f; // [verified: bf3dump weapon defibrillator] MaxRange
	float corpse_up_m = 0.3f; // the corpse sphere's centre above the feet
	float corpse_radius_m = 0.6f;
	float living_radius_m = 0.6f; // a living soldier's sphere (centre 0.9 m up): a defib shot at it is refused
	// Scoring [verified: bf3dump scoring]; the loader replaces them with the data's rows when it reads them.
	int32_t score_kill = 100; // ID_SCORE_KILL
	int32_t score_headshot = 10; // ID_SCORE_HEADSHOT (on top of the kill)
	int32_t score_revive = 100; // ID_SCORE_REVIVE
	// Kits (§3.2): a bot's kit per life, weights of assault, engineer, support, recon.
	std::array<uint32_t, 4> bot_kit_weights{ 40, 20, 20, 20 };
	// Spawns (§3.5, sim::SpawnQuery).
	float spawn_safe_distance_m = 40.0f;
	float friendly_block_radius_m = 30.0f;
	int32_t friendly_block_count = 3; // [verified] SpawnProtectionFriendlyKilledCount
	float friendly_block_s = 30.0f; // [verified]
	int32_t spawn_los_rays = 16;
	float spawn_chest_height_m = 1.3f;
	// Teams.
	float team_kill_weight = 0.0f; // KillCounter TeamKillWeight is not decoded (Q15)
	bool rotate_teams = true; // AutoTeam RotateTeamOnNewRound at the restart (Q4)
	// Health (§3.3).
	bool suppression_resets_regen = false; // no suppression in M2b: no input
	// Spawn immunity (§3.3): the data's raw values -> command bits. Used when the EBX type info carries no enum
	// names; with names the loader maps Fire / Throttle / Strafe itself and notes it.
	std::vector<ImmunityAbortEntry> immunity_abort_map{ { 0, 0, true }, { 1, 0, true }, { 7, sim::buttons::kFire, false } };
	bool measured = false;
};
core::Result<TdmTuning> parse_tdm_tuning(std::string_view p_text);
// p_path empty: TdmTuning{}; otherwise the file (refused above kMaxTdmTuningBytes) through parse_tdm_tuning.
core::Result<TdmTuning> load_tdm_tuning(const std::string &p_path);

// GameServerConfig::rules (#1). Defaults: no rules (M2a).
struct MatchRulesConfig {
	enum class Mode : uint8_t { None = 0, Tdm = 1 };
	Mode mode = Mode::None;
	bool deploy_screen = false; // humans spawn on an accepted SpawnRequest only (R10); off: M2a's Ready spawn, KitSelect kit
	bool revive = true; // the defibrillator revives (§3.6)
	int kill_limit_override = 0; // > 0 replaces KillCounter.MaxKillCount (--kill-limit; dev, Q12)
	float preround_override_s = -1.0f; // < 0: the data's countdown (10 s); 0: none (tests, --preround=0)
	uint8_t max_players = 32; // RoundNativeConfig::max_players (Metro TDM's cap; 64 for E8)
};

// What the rules ask the GameServer to do this tick; executed by Impl::sv_orders in [orders] (#20), in vector order.
struct RulesOrder {
	enum class Kind : uint8_t { Spawn = 0, Revive = 1, Unspawn = 2 };
	Kind kind = Kind::Spawn;
	uint8_t slot = 0;
	sim::Vec3 position{}; // Revive only: the corpse
	uint16_t yaw = 0; // Revive only
	float health = 0; // Revive only: SoldierDef post_revive_health (20)
	uint8_t reviver = 0; // M2b ru (as built): Revive only, the shooter (ReviveEvent::reviver)
};

// Why a SpawnRequest was refused (counted in RulesStats::refused, otherwise silent: the spawn screen shows the timer).
enum class SpawnRefusal : uint8_t { None = 0, Alive = 1, TooEarly = 2, BadKit = 3, Phase = 4, RateLimited = 5, Spectator = 6 };
inline constexpr size_t kSpawnRefusalCount = 7;

// kit_of: keep the slot's KitSelect kit (a remote slot without the deploy screen, R7).
inline constexpr uint8_t kKeepKit = 0xFF;

// What the rules counted (server thread; tests read it between ticks). Trivially copyable.
struct RulesStats {
	std::array<uint64_t, kSpawnRefusalCount> refused{}; // by SpawnRefusal (index None unused)
	uint64_t spawn_requests = 0, spawns_accepted = 0;
	uint64_t spawn_fallbacks = 0; // R1: the scored chooser found nothing, the M2a chooser picked (E2 counts them apart)
	uint64_t revives = 0;
	uint64_t defib_shots = 0; // defib shots tested
	uint64_t defib_misses = 0; // no downed teammate on the segment
	uint64_t defib_refused = 0; // an enemy shooter, a living target or an expired man-down
	uint64_t regen_ticks = 0; // slot-ticks with regen applied
	uint64_t networked_dropped = 0; // LogicRuntime::networked() events collected and dropped (M3 sends them, §3.8)
	uint64_t unspawns = 0;
	uint64_t restarts = 0;
	uint64_t balance_moves = 0; // AutoTeam moves (applied at the moved bot's next death)
	uint64_t restricted_commands = 0; // commands the input restriction changed ([cmd])
	uint32_t rounds = 0; // RoundOver reached
	// M2b ru (as built): E2's spawn counts (the chooser is const; counted through the Impl).
	uint32_t reserved0 = 0;
	uint64_t spawns_scored = 0; // R1 picks by choose_spawn_scored
	uint64_t spawns_enemy_within_10m = 0; // scored picks with a living enemy within 10 m (E2: 0)
	uint64_t runtime_faults = 0; // the LogicRuntime or the natives failed a step (the rules then stop the round flow)
};
static_assert(std::is_trivially_copyable_v<RulesStats>);

class TdmRules final : public RespawnPolicy, public CombatEventSink {
public:
	// p_bots: GameServerConfig::bots->level (may be null; the bots' nav and their spawn list). Fails without
	// SharedLevel::logic or gunplay, or when the graph's natives do not build (RoundNatives::create).
	static core::Result<std::unique_ptr<TdmRules>> create(const SharedLevel &p_level, std::shared_ptr<const BotLevel> p_bots,
			const MatchRulesConfig &p_config, uint64_t p_match_seed, uint32_t p_max_slots);
	~TdmRules() override;
	TdmRules(const TdmRules &) = delete;
	TdmRules &operator=(const TdmRules &) = delete;

	// RespawnPolicy (asked through sv_policy() by Sink::death, R8, and the fall death, R3): bots and humans without the
	// deploy screen get death + PlayerKilledDelay, or the longer revive wait of §3.4 for a bot near an assault
	// teammate; a deploy-screen human and every death in RoundOver / Restarting get nullopt (the spawn then comes only
	// as a RulesOrder::Spawn).
	std::optional<uint32_t> respawn_tick(uint8_t p_slot, uint32_t p_death_tick) override;
	uint32_t earliest_respawn(uint8_t p_slot) const; // what R8 stamps into DeathEvent::respawn_tick (also for nullopt)
	// What R8 stamps into DeathEvent::revive_until (0: not revivable); for an auto-respawning slot
	// min(man-down end, its respawn tick).
	uint32_t revive_until(uint8_t p_slot) const;

	// CombatEventSink (the GameServer's Sink forwards after its own bookkeeping, R8).
	void hit(const net::HitEvent &p_event) override;
	void death(const net::DeathEvent &p_event) override;
	void detonation(const net::DetonationWire &p_event) override;
	void revive(const net::ReviveEvent &p_event) override;

	uint8_t on_connect(uint8_t p_slot, bool p_bot, std::string_view p_name); // returns AutoTeam's team (1 / 2); [add], R2
	void on_disconnect(uint8_t p_slot);
	void on_spawned(uint8_t p_slot, uint32_t p_tick); // every spawn (R7): leaves the downed list, HumanPlayer.OnPlayerSpawned
	SpawnRefusal request_spawn(uint8_t p_slot, uint8_t p_kit, uint8_t p_spawn_choice, uint32_t p_tick); // [recv]

	// [rules], inside sv_rules after apply_rules and before the copy-back.
	core::Status step(uint32_t p_tick, const ServerView &p_view, const Gunplay &p_gunplay, std::span<sim::SoldierState> r_states,
			std::vector<RulesOrder> &r_orders);

	// §3.5 (R1). p_points is the list spawn() chose for the slot (game_server.cpp:227-230); the result indexes it.
	// SIZE_MAX = use the M2a chooser (counted as a fallback).
	size_t choose_spawn(uint8_t p_slot, std::span<const sim::SpawnPoint> p_points, const ServerView &p_view, uint32_t p_tick) const;

	// [cmd]: net::restrict_command over the current MatchState (a refused bit or axis reads as released for a command
	// whose tick is below restrict_until_tick); the server and the client apply the same function.
	sim::Command apply_input_restriction(const sim::Command &p_command) const;

	std::span<const DownedSoldier> downed() const; // [view]
	net::MatchPhase phase() const;
	bool input_restricted() const;
	const net::MatchState &match_state() const; // [snap]
	std::span<const net::PlayerRow> players() const; // ascending slot, every connected player
	bool match_state_changed() const; // since the last mark_sent ([snap]: send on change, §3.7)
	bool players_changed() const;
	void mark_sent(); // [snap] after the send step
	void mark_state_sent(); // M2b ru (as built): the MatchState half of mark_sent (the rows go at most every 15 ticks)
	void mark_players_sent(); // M2b ru: the rows half
	uint8_t team_of(uint8_t p_slot) const; // AutoTeam's team, including a balance move or the round's rotation (R7)
	uint8_t kit_of(uint8_t p_slot) const; // SpawnRequest's kit, the bot draw, or kKeepKit (R7)
	const sim::logic::LogicTrace &trace() const; // E6 (logic_runtime.h)
	const RulesStats &stats() const; // refusals, fallbacks, revives (E2, E7)
	// M2b ru (as built, docs/design/m2b-ru.md §4).
	// [snap]: the whole list (p_full, a new client) or the rows changed and the slots removed since mark_sent.
	net::PlayerList player_list(sim::Tick p_server_tick, bool p_full) const;
	bool faulted() const; // a runtime or natives step failed (the error went to stderr once; stats().runtime_faults)
	const sim::logic::RoundNatives &natives() const; // E1's coverage(), the timeline

private:
	TdmRules();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host

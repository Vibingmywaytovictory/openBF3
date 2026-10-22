#pragma once

#include "bf3/core/result.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/logic/round_natives.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::host {

struct BotLevel;

inline constexpr std::string_view kTdmTuningPath = "config/tdm_tuning.cfg";
inline constexpr size_t kMaxTdmTuningBytes = 64 * 1024;
inline constexpr size_t kMaxTdmTuningLines = 1024;
inline constexpr size_t kMaxImmunityAbortEntries = 64;
struct ImmunityAbortEntry {
	int32_t value = 0;
	uint32_t buttons = 0;
	bool move = false;
};
struct TdmTuning {

	float revive_wait_radius_m = 20.0f;
	float revive_wait_s = 8.0f;
	float restart_estimate_s = 60.0f;

	bool revive_takes_kill = true;
	float defib_range_m = 1.8f;
	float corpse_up_m = 0.3f;
	float corpse_radius_m = 0.6f;
	float living_radius_m = 0.6f;

	int32_t score_kill = 100;
	int32_t score_headshot = 10;
	int32_t score_revive = 100;

	std::array<uint32_t, 4> bot_kit_weights{ 40, 20, 20, 20 };

	float spawn_safe_distance_m = 40.0f;
	float friendly_block_radius_m = 30.0f;
	int32_t friendly_block_count = 3;
	float friendly_block_s = 30.0f;
	int32_t spawn_los_rays = 16;
	float spawn_chest_height_m = 1.3f;

	float team_kill_weight = 0.0f;
	bool rotate_teams = true;

	bool suppression_resets_regen = false;

	std::vector<ImmunityAbortEntry> immunity_abort_map{ { 0, 0, true }, { 1, 0, true }, { 7, sim::buttons::kFire, false } };
	bool measured = false;
};
core::Result<TdmTuning> parse_tdm_tuning(std::string_view p_text);

core::Result<TdmTuning> load_tdm_tuning(const std::string &p_path);

struct MatchRulesConfig {
	enum class Mode : uint8_t { None = 0, Tdm = 1 };
	Mode mode = Mode::None;
	bool deploy_screen = false;
	bool revive = true;
	int kill_limit_override = 0;
	float preround_override_s = -1.0f;
	int min_players_override = -1;
	uint8_t max_players = 32;
};

struct RulesOrder {
	enum class Kind : uint8_t { Spawn = 0, Revive = 1, Unspawn = 2 };
	Kind kind = Kind::Spawn;
	uint8_t slot = 0;
	sim::Vec3 position{};
	uint16_t yaw = 0;
	float health = 0;
	uint8_t reviver = 0;
};

enum class SpawnRefusal : uint8_t { None = 0, Alive = 1, TooEarly = 2, BadKit = 3, Phase = 4, RateLimited = 5, Spectator = 6 };
inline constexpr size_t kSpawnRefusalCount = 7;

inline constexpr uint8_t kKeepKit = 0xFF;

struct RulesStats {
	std::array<uint64_t, kSpawnRefusalCount> refused{};
	uint64_t spawn_requests = 0, spawns_accepted = 0;
	uint64_t spawn_fallbacks = 0;
	uint64_t revives = 0;
	uint64_t defib_shots = 0;
	uint64_t defib_misses = 0;
	uint64_t defib_refused = 0;
	uint64_t regen_ticks = 0;
	uint64_t networked_dropped = 0;
	uint64_t unspawns = 0;
	uint64_t restarts = 0;
	uint64_t balance_moves = 0;
	uint64_t restricted_commands = 0;
	uint32_t rounds = 0;

	uint32_t reserved0 = 0;
	uint64_t spawns_scored = 0;
	uint64_t spawns_enemy_within_10m = 0;
	uint64_t runtime_faults = 0;
};
static_assert(std::is_trivially_copyable_v<RulesStats>);

class TdmRules final : public RespawnPolicy, public CombatEventSink {
public:

	static core::Result<std::unique_ptr<TdmRules>> create(const SharedLevel &p_level, std::shared_ptr<const BotLevel> p_bots,
			const MatchRulesConfig &p_config, uint64_t p_match_seed, uint32_t p_max_slots);
	~TdmRules() override;
	TdmRules(const TdmRules &) = delete;
	TdmRules &operator=(const TdmRules &) = delete;

	std::optional<uint32_t> respawn_tick(uint8_t p_slot, uint32_t p_death_tick) override;
	uint32_t earliest_respawn(uint8_t p_slot) const;

	uint32_t revive_until(uint8_t p_slot) const;

	void hit(const net::HitEvent &p_event) override;
	void death(const net::DeathEvent &p_event) override;
	void detonation(const net::DetonationWire &p_event) override;
	void revive(const net::ReviveEvent &p_event) override;

	uint8_t on_connect(uint8_t p_slot, bool p_bot, std::string_view p_name);
	void on_disconnect(uint8_t p_slot);
	void on_spawned(uint8_t p_slot, uint32_t p_tick);
	SpawnRefusal request_spawn(uint8_t p_slot, uint8_t p_kit, uint8_t p_spawn_choice, uint32_t p_tick);

	core::Status step(uint32_t p_tick, const ServerView &p_view, const Gunplay &p_gunplay, std::span<sim::SoldierState> r_states,
			std::vector<RulesOrder> &r_orders);

	size_t choose_spawn(uint8_t p_slot, std::span<const sim::SpawnPoint> p_points, const ServerView &p_view, uint32_t p_tick) const;

	sim::Command apply_input_restriction(const sim::Command &p_command) const;

	std::span<const DownedSoldier> downed() const;
	net::MatchPhase phase() const;
	bool input_restricted() const;
	const net::MatchState &match_state() const;
	std::span<const net::PlayerRow> players() const;
	bool match_state_changed() const;
	bool players_changed() const;
	void mark_sent();
	void mark_state_sent();
	void mark_players_sent();
	uint8_t team_of(uint8_t p_slot) const;
	uint8_t kit_of(uint8_t p_slot) const;
	const sim::logic::LogicTrace &trace() const;
	const RulesStats &stats() const;

	net::PlayerList player_list(sim::Tick p_server_tick, bool p_full) const;
	bool faulted() const;
	const sim::logic::RoundNatives &natives() const;

private:
	TdmRules();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}

#pragma once

#include "bf3/core/result.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/host/squads.h"
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

	bool revive_takes_kill = true;
	float defib_range_m = 1.8f;
	float corpse_up_m = 0.3f;
	float corpse_radius_m = 0.6f;
	float living_radius_m = 0.6f;

	int32_t score_kill = 100;
	int32_t score_headshot = 10;
	int32_t score_revive = 100;
	int32_t score_spawn_squad = 10;
	int32_t score_revive_squad = 110;
	int32_t score_squad_wipe = 10;

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
	bool hold_level_end = false;
};

struct RulesOrder {
	enum class Kind : uint8_t { Spawn = 0, Revive = 1, Unspawn = 2, SpawnAt = 3 };
	Kind kind = Kind::Spawn;
	uint8_t slot = 0;
	sim::Vec3 position{};
	uint16_t yaw = 0;
	float health = 0;
	uint8_t reviver = 0;
};

enum class SpawnRefusal : uint8_t {
	None = 0,
	Alive = 1,
	TooEarly = 2,
	BadKit = 3,
	Phase = 4,
	RateLimited = 5,
	Spectator = 6,
	SquadMate = 7
};
inline constexpr size_t kSpawnRefusalCount = 8;
inline constexpr sim::Vec3 kSquadSpawnNavExtents{ 1.0f, 2.0f, 1.0f };

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
	uint64_t suicides = 0, suicides_refused = 0;
	uint64_t squad_requests = 0, squad_refused = 0;
	uint64_t team_switches = 0, team_switches_refused = 0;
	uint64_t squad_spawns = 0, squad_spawns_on_nav = 0;
	uint64_t squad_revives = 0, squad_wipes = 0;
	uint64_t level_ends = 0;
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
	uint8_t on_carry(uint8_t p_slot, std::string_view p_name, uint8_t p_team, int p_squad_mate);
	void on_disconnect(uint8_t p_slot);
	bool level_ended() const;
	void restart_in_place(uint32_t p_tick, std::vector<RulesOrder> &r_orders);
	void on_spawned(uint8_t p_slot, uint32_t p_tick);
	SpawnRefusal request_spawn(uint8_t p_slot, uint8_t p_kit, uint8_t p_spawn_choice, uint32_t p_tick);
	bool request_suicide(uint8_t p_slot);
	bool request_revive(uint8_t p_slot);
	bool take_suicide(uint8_t p_slot);
	SquadRefusal request_squad(uint8_t p_slot, const net::SquadRequest &p_request);
	bool set_squad_order(uint8_t p_team, uint8_t p_squad, const SquadOrderState &p_order);
	void set_spawn_choice(uint8_t p_slot, uint8_t p_choice);
	bool take_squad_spawn(
			uint8_t p_slot, std::span<const sim::SoldierState> p_live, sim::Vec3 &r_position, uint16_t &r_yaw);
	const SquadTable &squads() const;

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
	std::span<const sim::logic::LogicNetworkedEvent> networked() const;
	std::optional<sim::logic::LogicGhost> ghost() const;

private:
	TdmRules();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}

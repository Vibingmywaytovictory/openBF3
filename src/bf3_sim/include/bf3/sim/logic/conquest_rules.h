#pragma once

#include "bf3/sim/sim_types.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::sim::logic {

inline constexpr size_t kConquestTeams = 3;
inline constexpr size_t kConquestSlots = 256;
inline constexpr float kFlagTop = 100.0f;

enum class CapturableType : uint8_t { Default = 0, UnableToChangeTeam = 1, CaptureEnablable = 2 };

struct CaptureVolume {
	std::vector<std::array<float, 2>> polygon;
};

struct CapturePointParams {
	uint8_t initial_owner = 0;
	CapturableType type = CapturableType::Default;
	Vec3 position{};
	float capture_radius = 0.0f;
	bool upper_sphere = false;
	std::vector<CaptureVolume> volumes;
	float area_value = 0.0f;
	bool area_values_per_team = false;
	std::array<float, kConquestTeams> area_values{};
	int32_t time_to_get_control = 20;
	int32_t time_to_lose_control = 20;
	float return_multiplier = 1.0f;
	uint8_t only_takeable_by_team = 0;
	int32_t min_nr_to_take_control = 1;
	int32_t max_capture_multiplier = 4;
	float added_multiplier_per_player = 0.34f;
	bool seesaw = true;
	bool start_at_bottom = true;
	bool lose_control_when_not_close = false;
	bool disable_if_enemy_inside = false;
	bool intruder_warning = false;
	bool enabled = true;
	int32_t spawn_menu_list_ordinal = 0;

	float area_value_for(uint8_t p_team) const;
};

struct CapturePointState {
	uint8_t team = 0;
	uint8_t controller = 0;
	uint8_t attacker = 0;
	float progress = 0.0f;
	float home = 0.0f;
	float rate = 0.0f;
	bool controlled = false;
	bool under_attack = false;
	bool enabled = true;
	uint32_t previous_attackers = 0;
};

struct CaptureEvents {
	bool captured = false;
	bool lost = false;
	bool enable_spawners = false;
	bool disable_spawners = false;
	bool intruder = false;
	bool team_changed = false;
};

bool polygon_contains(std::span<const std::array<float, 2>> p_polygon, float p_x, float p_z);

CapturePointState reset_capture_point(const CapturePointParams &p_params);

bool capture_point_contains(const CapturePointParams &p_params, const Vec3 &p_point);

CaptureEvents step_capture_point(const CapturePointParams &p_params, CapturePointState &r_state,
		std::span<const uint32_t, kConquestTeams> p_inside, float p_dt);

struct SpawnerGate {
	uint8_t data_team = 0;
	bool locked_team = false;
	uint8_t team = 0;
	bool enabled = false;
};

void enable_spawner(SpawnerGate &r_gate, uint8_t p_flag_team);
void disable_spawner(SpawnerGate &r_gate);

enum class TicketDecrease : uint8_t { OnKilled = 0, OnRespawned = 1, NoDecreaseOnSoldierDeath = 2 };

struct TicketPercentage {
	float max_percent_of_tickets_left = 0.0f;
	float ticket_loss_per_min = 0.0f;
};

struct TicketCounterParams {
	uint8_t team = 0;
	int64_t initial_ticket_count = 0;
	TicketDecrease decrease = TicketDecrease::OnRespawned;
	float loss_per_min = 0.0f;
	float loss_start = 0.0f;
	float loss_when_lost_all = 0.0f;
	float loss_when_lost_all_controls_point = 0.0f;
	std::vector<TicketPercentage> percentages;
	bool halt_on_equal_point_count = false;
	bool reset_capture_points_on_reset = false;
	bool spawn_always_allowed = false;
};

struct TicketCounterState {
	double initial = 0.0;
	double tickets = 0.0;
	double area = 0.0;
	bool round_started = false;
	bool respawn_seen = false;
	bool area_counted = false;
	bool bleeding = false;
	bool all_held = false;
	bool stopped = false;
	size_t percentage_step = 0;
	std::bitset<kConquestSlots> spawned;

	int64_t displayed() const;
	bool losing() const { return round_started && respawn_seen && !stopped && tickets > 0.0; }
};

struct TicketEvents {
	bool start_bleed = false;
	bool stop_bleed = false;
	bool all_held = false;
	bool percentage_step = false;
};

double initial_tickets(int64_t p_initial_ticket_count, double p_game_mode_counter_modifier);

TicketCounterState reset_ticket_counter(const TicketCounterParams &p_params, double p_game_mode_counter_modifier);

void ticket_respawn(const TicketCounterParams &p_params, TicketCounterState &r_state, uint8_t p_slot, uint8_t p_team);
void ticket_killed(const TicketCounterParams &p_params, TicketCounterState &r_state, uint8_t p_team, bool p_suppressed);
void ticket_left(const TicketCounterParams &p_params, TicketCounterState &r_state, uint8_t p_slot, uint8_t p_team);
void ticket_captured(const TicketCounterParams &p_params, TicketCounterState &r_state,
		const CapturePointParams &p_point, const CapturePointState &p_state);
void ticket_lost(const TicketCounterParams &p_params, TicketCounterState &r_state, const CapturePointParams &p_point,
		const CapturePointState &p_state);

struct ConquestPoint {
	const CapturePointParams *params = nullptr;
	const CapturePointState *state = nullptr;
};

void ticket_count_area(
		const TicketCounterParams &p_params, TicketCounterState &r_state, std::span<const ConquestPoint> p_points);

struct BleedInputs {
	double total_area = 0.0;
	bool holds_point = false;
	bool soldier_alive = false;
	bool enemy_holds_all = false;
	bool bleed_disabled = false;
	double bleed_scale = 1.0;
};

bool team_holds_point(uint8_t p_team, std::span<const ConquestPoint> p_points);
bool enemy_holds_all(uint8_t p_team, std::span<const ConquestPoint> p_points);
bool spawn_allowed(const TicketCounterParams &p_params, uint8_t p_team, std::span<const ConquestPoint> p_points);

TicketEvents ticket_bleed(
		const TicketCounterParams &p_params, TicketCounterState &r_state, const BleedInputs &p_inputs, double p_dt);

struct CombatAreaParams {
	uint8_t team = 0;
	bool team_specific = false;
	bool ai_allowed_outside = true;
	int32_t time_to_return = 10;
};

struct CombatAreaPlayer {
	uint8_t slot = 0;
	uint8_t team = 0;
	bool alive = false;
	bool ai = false;
	bool inside = false;
};

struct Deserter {
	uint8_t slot = 0;
	float timer = 0.0f;
	bool struck = false;
};

struct CombatAreaEvents {
	std::vector<Deserter> entered;
	std::vector<uint8_t> returned;
	std::vector<uint8_t> strikes;
	std::vector<uint8_t> kills;
};

bool combat_area_tracks(const CombatAreaParams &p_params, const CombatAreaPlayer &p_player, bool p_deserting_allowed);

CombatAreaEvents step_combat_area(const CombatAreaParams &p_params, std::vector<Deserter> &r_deserters,
		std::span<const CombatAreaPlayer> p_players, bool p_deserting_allowed, float p_dt);

}

#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

struct TicketCounterDef {
	int team = 0;
	int64_t initial_tickets = 0;
	double loss_per_min = 0;
	double loss_start = 0;
	double loss_when_lost_all = 0;
	double loss_when_lost_all_control_points = 0;
	bool halt_on_equal_point_count = false;
	std::string decrease_tickets;
	std::string prefab;
};

struct CapturePointDef {
	std::string prefab;
	std::string location_sid;
	std::string capturable_type;
	int initial_owner = 0;
	double area_value = 0;
	double time_to_get_control = 0;
	double time_to_lose_control = 0;
	double added_multiplier_per_player = 0;
	double max_capture_multiplier = 0;
	double return_multiplier = 0;
	double capture_radius = 0;
	int64_t min_nr_to_take_control = 0;
	int64_t enemy_ticket_loss_when_captured = 0;
	bool seesaw = false;
	bool enabled = true;

	bool is_hq() const { return capturable_type == "CTUnableToChangeTeam"; }
};

struct LifeCounterDef {
	int team = 0;
	bool attacker = false;
	int64_t starting_lives = 0;
	int64_t base_count = 0;
	double fallback_time = 0;
	bool simple_reinforce = false;
	std::string prefab;
};

struct KillCounterDef {
	int team = 0;
	int64_t max_kills = 0;
	double enemy_weight = 0;
	std::string type;
	std::string prefab;
};

struct McomDef {
	std::string prefab;
	std::optional<double> arm_time;
	std::optional<double> disarm_time;
	std::optional<double> radius;
	std::optional<double> fuse;
	std::optional<bool> long_fuse_switch;
};

struct VehicleSpawnDef {
	std::string blueprint;
	int team = 0;
	double spawn_delay = 0;
	double initial_spawn_delay = 0;
	bool enabled = true;
};

struct GameModeTeamSizeDef {
	int64_t players = 0;
	int64_t squad_size = 0;
};

struct GameModeSizeDef {
	std::string name;
	std::string short_name;
	bool fallback = false;
	int64_t player_count = 0;
	int64_t rounds_per_map = 0;
	bool force_squad = false;
	std::vector<GameModeTeamSizeDef> teams;

	int64_t squad_size() const;
};

struct InterfaceConstantDef {
	std::string subworld;
	int32_t id = 0;
	std::string name;
	std::string value;
};

struct ModeDef {
	std::string level_id;
	std::string mode_id;
	std::vector<std::string> subworlds;
	std::vector<TeamDef> teams;
	std::vector<TicketCounterDef> ticket_counters;
	std::vector<CapturePointDef> capture_points;
	std::vector<LifeCounterDef> life_counters;
	std::vector<KillCounterDef> kill_counters;
	std::vector<McomDef> mcoms;
	std::vector<VehicleSpawnDef> vehicle_spawns;
	std::optional<GameModeSizeDef> size;
	std::vector<InterfaceConstantDef> interface_constants;
	size_t scopes = 0;
	size_t objects = 0;
	size_t walk_steps = 0;
	size_t query_steps = 0;
	size_t work = 0;
	size_t unresolved_connections = 0;

	const TicketCounterDef *tickets(int p_team) const;
	const LifeCounterDef *lives(int p_team) const;
	const KillCounterDef *kills(int p_team) const;
	const TeamDef *team(int p_team) const;

	size_t flag_count() const;

	std::optional<double> flag_area_value() const;
};

Result<std::vector<std::string>> level_modes(const EbxResolver &p_db, std::string_view p_level_id, WorkBudget *p_budget = nullptr);

Result<ModeDef> read_mode_def(const EbxResolver &p_db, std::string_view p_level_id, std::string_view p_mode, WorkBudget *p_budget = nullptr);

struct ScoringEntryDef {
	std::string sid;
	double score = 0;
	std::string measuring;
	std::string bucket;
};

struct ScoringTable {
	std::vector<ScoringEntryDef> entries;

	std::optional<double> score(std::string_view p_sid) const;
};

Result<ScoringTable> read_scoring(const EbxResolver &p_db, std::string_view p_partition = "persistence/scoring/scoring", WorkBudget *p_budget = nullptr);

}

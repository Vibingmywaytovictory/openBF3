#pragma once
// ModeDef: the mode constants of one level x mode, resolved through the level's interfaces and
// PropertyConnections (plan §3.7, §7.3; research/topics/02-soldier-kits-modes.md §3;
// research/topics/gap-r1-level-logic-schematics.md).
//
// The mode's sub-worlds are the SubWorldReferenceObjectData of the LevelData whose GameMode
// inclusion settings enable the mode (levels/<id>/<id>, AutoLoad). A LogicWorld walks them and the
// prefabs they place; the gameplay entities found there (TeamEntityData, TicketCounterEntityData,
// CapturePointEntityData, LifeCounterEntityData, KillCounterEntityData, GameInteractionEntityData,
// VehicleSpawnReferenceObjectData) are read with their effective field values: a value a
// PropertyConnection feeds (an interface constant of the level's sub-world, e.g. CQS
// InitialTicketCount 250) wins over the entity's own field. Player counts and squad sizes come from
// levels/<id>/<id>/settings_win32 (GameModeSettings, Win32 platform).
//
// Rush MCOM fuse: the crate prefab arms a DelayTriggerEntityData through one of two EventGates,
// opened or closed by a SyncedBoolEntityData whose In is an interface field (25 s, or 45 s when the
// bool is true, topics/02 §3.4). The reader evaluates that gate choice; it reports no fuse when the
// crate is wired differently.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
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
	std::string decrease_tickets; // TDTOnRespawned
	std::string prefab;
};

struct CapturePointDef {
	std::string prefab; // the prefab scope it sits in, e.g. gameplay/level_setups/components/capturepointprefab
	std::string location_sid; // LocationTextSid of the enclosing interfaces, when one is set
	std::string capturable_type; // CTDefault, CTUnableToChangeTeam (HQ)
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
	std::string type; // KillCounterEntityData or a derived type
	std::string prefab;
};

// One Rush crate (MCOM) instance.
struct McomDef {
	std::string prefab;
	std::optional<double> arm_time; // IET_RushCrateArm HoldToInteractTime
	std::optional<double> disarm_time; // IET_RushCrateDisarm HoldToInteractTime
	std::optional<double> radius; // arm UseWithinRadius
	std::optional<double> fuse; // see the header comment
	std::optional<bool> long_fuse_switch; // the bool that picks the fuse
};

struct VehicleSpawnDef {
	std::string blueprint; // VehicleBlueprint partition
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
	std::string name; // ConquestSmall
	std::string short_name; // CQS
	bool fallback = false; // no size for the mode: the DefaultSize was used
	int64_t player_count = 0;
	int64_t rounds_per_map = 0;
	bool force_squad = false;
	std::vector<GameModeTeamSizeDef> teams; // index 0 = neutral

	// The squad size of the playing teams (the first non-zero), 0 when none is set.
	int64_t squad_size() const;
};

struct InterfaceConstantDef {
	std::string subworld; // sub-world partition
	int32_t id = 0;
	std::string name; // the field name when a known gameplay name hashes to id, else empty
	std::string value; // DataField.Value as written
};

struct ModeDef {
	std::string level_id; // MP_001
	std::string mode_id; // ConquestSmall0
	std::vector<std::string> subworlds; // included sub-world bundles
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
	size_t unresolved_connections = 0;

	const TicketCounterDef *tickets(int p_team) const;
	const LifeCounterDef *lives(int p_team) const;
	const KillCounterDef *kills(int p_team) const;
	const TeamDef *team(int p_team) const;
	// Capture points that can change owner (HQs excluded).
	size_t flag_count() const;
	// The AreaValue shared by the flags; nullopt when there are none or they differ.
	std::optional<double> flag_area_value() const;
};

// "CQS" -> "ConquestSmall0" etc. (CQL, CQS, CQAL, CQAS, RL, SR/SQR, TDM, TDMC, SDM/SQDM, DOM, GM,
// TS, SCV, CTF, AS); anything else is returned unchanged.
std::string canonical_mode_id(std::string_view p_mode);
// The GameMode options levels/<id>/<id>/description_win32 lists for the level.
std::vector<std::string> level_modes(const EbxResolver &p_db, std::string_view p_level_id);
Result<ModeDef> read_mode_def(const EbxResolver &p_db, std::string_view p_level_id, std::string_view p_mode);

struct ScoringEntryDef {
	std::string sid; // DescriptionSid, e.g. ID_SCORE_KILL
	double score = 0;
	std::string measuring; // StatEvent_*
	std::string bucket;
};

struct ScoringTable {
	std::vector<ScoringEntryDef> entries;
	// The first entry with that DescriptionSid.
	std::optional<double> score(std::string_view p_sid) const;
};

// The ScoringTypeData of a ScoringData partition (persistence/scoring/scoring).
Result<ScoringTable> read_scoring(const EbxResolver &p_db, std::string_view p_partition = "persistence/scoring/scoring");

} // namespace bf3::core::gameplay

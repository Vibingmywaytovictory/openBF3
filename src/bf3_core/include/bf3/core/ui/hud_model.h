#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::core::ui {

struct HudSourceTable;

inline constexpr uint32_t kHudModelVersion = 1;

inline constexpr size_t kHudMaxPlayers = 64;
inline constexpr size_t kHudMaxCapturePoints = 16;
inline constexpr size_t kHudMaxSpawnPoints = 64;
inline constexpr size_t kHudMaxSquads = 32;
inline constexpr size_t kHudSquadSize = 4;
inline constexpr size_t kHudKillFeedSize = 8;
inline constexpr size_t kHudChatLines = 8;
inline constexpr size_t kHudNameBytes = 32;
inline constexpr size_t kHudTextBytes = 128;
inline constexpr uint8_t kHudNoPlayer = 0xFF;

using HudPlayerName = std::array<char, kHudNameBytes>;
using HudText = std::array<char, kHudTextBytes>;

struct HudWeaponSlot {
	uint32_t item_id = 0;
	uint32_t name_sid_hash = 0;
	int32_t weapon_class = 0;
	uint32_t fire_modes = 0;
};

struct HudPlayer {
	HudPlayerName name{};
	int8_t team = 0;
	int8_t squad = 0;
	int8_t kit = 0;
	bool alive = false;
	bool local = false;
	bool bot = false;
	int16_t kills = 0;
	int16_t deaths = 0;
	int16_t kit_icon = 0;
	int32_t score = 0;
	uint16_t ping_ms = 0;
	int16_t booster_icon = 0;
};

struct HudPlayerList {
	uint8_t count = 0;
	std::array<uint8_t, kHudMaxPlayers> index{};
};

struct HudCapturePoint {
	float x = 0.0f;
	float z = 0.0f;
	int8_t owner = 0;
	int8_t attacker = 0;
	bool hq = false;
	bool contested = false;
	float progress = 0.0f;
	uint32_t label_sid_hash = 0;
	uint32_t location_sid_hash = 0;
};

struct HudCapturePointList {
	uint8_t count = 0;
	std::array<HudCapturePoint, kHudMaxCapturePoints> points{};
};

struct HudTeamKillList {
	uint8_t count = 0;
	std::array<int32_t, 4> kills{};
};

struct HudKill {
	uint32_t serial = 0;
	uint8_t killer = kHudNoPlayer;
	uint8_t victim = kHudNoPlayer;
	uint32_t item_id = 0;
	uint32_t weapon_name_sid_hash = 0;
	uint32_t vehicle_item_hash = 0;
	bool headshot = false;
	bool suicide = false;
	bool team_kill = false;
};

struct HudKillFeed {
	uint32_t newest_serial = 0;
	std::array<HudKill, kHudKillFeedSize> lines{};
};

struct HudChatLine {
	uint32_t serial = 0;
	uint8_t player = kHudNoPlayer;
	uint8_t channel = 0;
	HudText text{};
};

struct HudTextFeed {
	uint32_t newest_serial = 0;
	std::array<HudChatLine, kHudChatLines> lines{};
};

struct HudTextMessage {
	uint32_t serial = 0;
	uint32_t sid_hash = 0;
	float seconds = 0.0f;
	HudText text{};
};

struct HudScoreEvent {
	uint32_t serial = 0;
	uint32_t sid_hash = 0;
	int32_t points = 0;
};

inline constexpr size_t kHudScoreFeedSize = 8;

struct HudScoreFeed {
	uint32_t newest_serial = 0;
	std::array<HudScoreEvent, kHudScoreFeedSize> events{};
};

enum class HudSpawnKind : uint8_t {
	Hq,
	Flag,
	SquadMate,
	Beacon,
	Vehicle,
};

struct HudSpawnPoint {
	HudSpawnKind kind = HudSpawnKind::Hq;
	int8_t team = 0;
	bool available = false;
	float x = 0.0f;
	float z = 0.0f;
	float yaw = 0.0f;
	uint32_t label_sid_hash = 0;
	uint8_t player = kHudNoPlayer;
};

struct HudSpawnPointList {
	uint8_t count = 0;
	std::array<HudSpawnPoint, kHudMaxSpawnPoints> points{};
};

struct HudSquadMemberList {
	uint8_t count = 0;
	std::array<uint8_t, kHudSquadSize> index{};
	uint8_t leader = kHudNoPlayer;
};

struct HudSquadEntry {
	int8_t squad = 0;
	uint8_t members = 0;
	bool open = true;
};

struct HudSquadList {
	uint8_t count = 0;
	std::array<HudSquadEntry, kHudMaxSquads> squads{};
};

struct HudWeapon {
	HudWeaponSlot active_weapon_data{};
	uint32_t active_weapon_crosshair = 0;
	int32_t active_weapon_ammo = 0;
	int32_t active_weapon_clips_total = 0;
	int32_t active_weapon_clips_left = 0;
	float active_weapon_reload_percent = 0.0f;
	int32_t active_weapon_ammo_in_bag = 0;
	int32_t active_weapon_clip_capacity = 0;
	bool active_weapon_critical_ammo = false;
	bool active_weapon_is_reloading = false;
	int32_t secondary_weapon_ammo = 0;
	int32_t secondary_weapon_clips_total = 0;
	int32_t secondary_weapon_clips_left = 0;
	float secondary_weapon_reload_percent = 0.0f;
	int32_t secondary_weapon_ammo_in_bag = 0;
	int32_t secondary_weapon_clip_capacity = 0;
	bool secondary_weapon_critical_ammo = false;
	bool secondary_weapon_is_reloading = false;
	HudWeaponSlot secondary_weapon_data{};
	bool show_hitindicator = false;
	float hitindicator_level = 0.0f;
	int32_t active_weapon_ammo_total = 0;
	int32_t secondary_weapon_ammo_total = 0;
	uint32_t active_weapon_name = 0;
	bool active_weapon_zoomed = false;
	int32_t active_weapon_index = 0;
	bool aim_on_teammate = false;
	bool aim_on_enemy = false;
	float dispersion = 0.0f;
	float absolute_dispersion = 0.0f;
	bool zoomed = false;
	float zoom_timer = 0.0f;
	float heat = 0.0f;
	bool heat_penalty = false;
	float range = 0.0f;
	float crosshair_scale = 0.0f;
	int32_t firing_mode = 0;
	uint32_t firing_mode_types = 0;
	bool active_weapon_show_ammo = false;
	bool show_crosshair = false;
	bool can_fire = false;
	float active_weapon_progress = 0.0f;
	float compass_angle = 0.0f;
	float weapon_pitch_angle = 0.0f;
	float range_meter = 0.0f;
	float time_to_live = 0.0f;
	uint32_t projectile_types = 0;
	int32_t projectile_index = 0;
	bool has_alternative_projectiles = false;
};

struct HudSoldier {
	bool is_alive = false;
	float supressing = 0.0f;
	int32_t pose = 0;
	float health = 0.0f;
	bool show_medic_revive = false;
	float camera_blur = 0.0f;
	bool first_person = false;
	float compass_angle = 0.0f;
	bool is_interactive_man_down = false;
	int32_t hud_state = 0;
	bool show_hud_ammo = false;
	bool show_hud_compass = false;
	float time_until_revive = 0.0f;
	float time_until_death = 0.0f;
	bool is_beeing_revived = false;
};

struct HudCapturePoints {
	int32_t active_capturepoints = 0;
	bool capture_in_progress = false;
	int32_t captured_by_team = 0;
	bool controlled = false;
	float capture_progress = 0.0f;
	int32_t friendly_capturepoints = 0;
	int32_t neutral_capturepoints = 0;
	int32_t enemy_capturepoints = 0;
	int32_t total_capturepoints = 0;
	HudCapturePointList capturepoints_status_data{};
	HudCapturePointList minimap_capturepoints_status_data{};
};

struct HudTickets {
	int32_t friendly_team_tickets = 0;
	int32_t enemy_team_tickets = 0;
	int32_t team1_tickets = 0;
	int32_t team2_tickets = 0;
	int32_t team1_max_tickets = 0;
	int32_t team2_max_tickets = 0;
	int32_t friendly_team_max_tickets = 0;
	int32_t enemy_team_max_tickets = 0;
	int32_t global_max_tickets = 0;
};

struct HudKillCounter {
	int32_t team1_kills = 0;
	int32_t team2_kills = 0;
	int32_t team3_kills = 0;
	int32_t team4_kills = 0;
	HudTeamKillList team_kills_list{};
	int32_t team1_max_kills = 0;
	int32_t team2_max_kills = 0;
	int32_t team3_max_kills = 0;
	int32_t team4_max_kills = 0;
	int32_t player_team_kills = 0;
	int32_t leading_team_kills = 0;
	int32_t player_team = 0;
	int32_t player_team_position = 0;
	int32_t leading_by_kills = 0;
	int32_t enemy_team_kills = 0;
	int32_t enemy_team1_kills = 0;
	int32_t enemy_team2_kills = 0;
	int32_t enemy_team3_kills = 0;
};

struct HudMessages {
	HudKillFeed kill_message{};
	HudTextFeed chat_message{};
	HudScoreEvent score_minor_message{};
	HudScoreEvent score_major_message{};
	HudTextMessage game_message{};
	HudTextMessage static_message{};
	HudTextMessage out_of_bounds_message{};
	HudScoreEvent kill_score_message{};
	HudKillFeed local_kill_message{};
	HudScoreFeed score_message{};
	int32_t score_aggregation = 0;
	HudTextMessage server_admin_yell_message{};
	HudTextMessage gun_master_message{};
};

struct HudMinimap {
	bool air_radar_mask_enabled = false;
	float radar_sweep_degree = 0.0f;
};

struct HudSpawnPoints {
	HudSpawnPointList spawn_points{};
	bool can_spawn = false;
	bool has_valid_spawn = false;
};

struct HudSpawnLogic {
	float time_to_respawn = 0.0f;
	float start_time_to_respawn = 0.0f;
	bool disable_suicide = false;
};

struct HudSquad {
	HudSquadMemberList squad_members{};
	int32_t squad_name = 0;
	bool local_player_is_squad_leader = false;
	int32_t squad_length = 0;
	bool local_player_has_squad = false;
	HudSquadList available_squads{};
	bool squad_is_empty = false;
	HudSquadMemberList local_squad_members{};
	bool leader_in_active_squad = false;
};

struct HudTeam {
	int32_t active_teams_data = 0;
	HudPlayerName top_player1_name{};
	HudPlayerName top_player2_name{};
	HudPlayerName top_player3_name{};
	HudPlayerList friendly_team_players{};
	HudPlayerList enemy_team1_players{};
	bool friendly_team_is_winner = false;
	bool enemy_team1_is_winner = false;
	uint32_t team1_name = 0;
	uint32_t team2_name = 0;
	uint32_t friendly_team_name = 0;
	uint32_t enemy_team1_name = 0;
	int32_t friendly_team = 0;
	int32_t enemy_team1 = 0;
	HudPlayerList team1_players{};
	HudPlayerList team2_players{};
	bool scoreboard_enabled = false;
	HudPlayerList sorted_players{};
};

inline constexpr size_t kHudTopPlayers = 3;
inline constexpr size_t kHudKitCount = 4;
inline constexpr size_t kHudSpawnKitItems = 5;

struct HudKillCard {
	uint32_t serial = 0;
	bool visible = false;
	uint8_t killer = kHudNoPlayer;
	int8_t killer_kit = -1;
	bool headshot = false;
	bool suicide = false;
	uint32_t item_id = 0;
	uint32_t weapon_name_sid_hash = 0;
	float distance_m = -1.0f;
	float killer_health = 0.0f;
	float seconds_left = 0.0f;

	HudPlayerName killer_name{};
	int32_t killer_rank = 0;
	int32_t killer_score = 0;
	uint32_t killer_weapon_name = 0;
	uint32_t killer_weapon_icon = 0;
	bool is_suicide = false;
	int32_t killer_player_kills = 0;
	int32_t player_killer_kills = 0;
	uint32_t killer_weapon_spec1_id = 0;
	uint32_t killer_weapon_spec1_name = 0;
	uint32_t killer_weapon_spec2_id = 0;
	uint32_t killer_weapon_spec2_name = 0;
	uint32_t killer_weapon_spec3_id = 0;
	uint32_t killer_weapon_spec3_name = 0;
	int32_t killer_weapon_type = 0;
	int32_t killer_vehicle_seat = -1;
	HudText killer_weapon_image_path{};
	HudText killer_rank_image_path{};
	int32_t killer_weapon_service_stars = 0;
	int32_t killer_kit_service_stars = 0;
	HudText killer_dogtag_image_path{};
	HudText killer_basic_dog_tag{};
	HudText killer_advanced_dog_tag{};
	bool is_bad_luck = false;
	bool is_pro = false;
};

enum class HudEorStage : uint8_t { None, Fade, PreEor, Scoreboard, Eor };

struct HudBestWeapon {
	uint32_t item_id = 0;
	uint32_t name_sid_hash = 0;
	int32_t kills = 0;
	HudText image_path{};
};

struct HudEndOfRound {
	bool visible = false;
	int8_t winner = 0;
	bool local_won = false;
	std::array<int32_t, 2> team_kills{};
	int32_t kill_limit = 0;
	std::array<uint8_t, kHudTopPlayers> top_players{ kHudNoPlayer, kHudNoPlayer, kHudNoPlayer };
	float seconds_to_next_round = 0.0f;

	HudEorStage stage = HudEorStage::None;
	float stage_seconds = 0.0f;
	int32_t winning_team = 0;
	bool local_player_in_winning_team = false;
	float time_until_level_change = 0.0f;
	int32_t combat_score = 0;
	int32_t total_score = 0;
	int32_t kills = 0;
	int32_t deaths = 0;
	float kill_death_ratio = 0.0f;
	HudBestWeapon best_weapon_info{};
	float round_duration = 0.0f;
	float kills_per_minute = 0.0f;
	int32_t longest_streak = 0;
	int32_t longest_nemesis_streak = 0;
	int32_t dogtags_taken = 0;
	float longest_headshot = 0.0f;
	float total_playtime = 0.0f;
	float score_per_minute = 0.0f;
	HudPlayerName name{};
	std::array<int32_t, 2> team_results{};
	int32_t player_team_index = 0;
	bool in_match = false;
	bool is_ranked = false;
	bool is_spectator = false;
};

struct HudSpawnScreen {
	bool visible = false;
	bool at_join = false;
	uint8_t selected_kit = 0;
	bool can_deploy = false;
	float seconds_to_deploy = 0.0f;
	bool revive_possible = false;
	float seconds_revive_left = 0.0f;

	std::array<std::array<uint32_t, kHudSpawnKitItems>, kHudKitCount> kit_items{};
};

struct HudPreRound {
	bool visible = false;
	bool counting_down = false;
	float seconds_left = 0.0f;
	bool input_restricted = false;

	int32_t count_left = 0;
	int32_t players_needed = 0;
};

inline constexpr size_t kHudDamageFeedSize = 8;

struct HudDamageHit {
	uint32_t serial = 0;
	uint8_t shooter = kHudNoPlayer;
	float bearing_deg = 0.0f;
	float damage = 0.0f;
};

struct HudDamageFeed {
	uint32_t newest_serial = 0;
	std::array<HudDamageHit, kHudDamageFeedSize> hits{};
};

inline constexpr size_t kHudGridItems = 32;
inline constexpr size_t kHudAccessorySlots = 4;
inline constexpr size_t kHudMenuEntries = 8;

struct HudItem {
	uint32_t item_id = 0;
	uint32_t name_sid_hash = 0;
	HudText image_path{};
};

struct HudItemList {
	uint8_t count = 0;
	std::array<HudItem, kHudGridItems> items{};
};

struct HudKitEntry {
	uint32_t name_sid_hash = 0;
	int32_t hud_icon = 0;
	std::array<HudItem, kHudSpawnKitItems> items{};
};

struct HudKit {
	int32_t selected_kit = 0;
	int32_t number_of_kits = int32_t(kHudKitCount);
	uint32_t selected_kit_name = 0;
};

struct HudCustomization {
	HudItem kit_primary_weapon{};
	HudItem kit_secondary_weapon{};
	HudItem kit_gadget1{};
	HudItem kit_gadget2{};
	HudItem kit_specialization{};
	HudItem weapon_accessory_main{};
	HudItem weapon_accessory1{};
	HudItem weapon_accessory2{};
	HudItem weapon_accessory3{};
	HudItem weapon_accessory4{};
	std::array<HudItemList, kHudSpawnKitItems> kit_grid_data{};
	std::array<int16_t, kHudSpawnKitItems> kit_grid_selected{};
	std::array<HudItemList, kHudAccessorySlots> accessory_grid{};
	std::array<int16_t, kHudAccessorySlots> accessory_selected{};
	std::array<HudKitEntry, kHudKitCount> kits_data{};
	int32_t customization_team = 0;
};

struct HudSettings {
	HudText current_game_mode{};
	uint32_t current_game_mode_name = 0;
	bool is_pc = true;
};

struct HudLevel {
	uint32_t current_level = 0;
};

enum class HudMenuAction : uint8_t { Resume, Scoreboard, Suicide, Options, Quit };

struct HudMenuEntry {
	uint32_t label_sid_hash = 0;
	HudMenuAction action = HudMenuAction::Resume;
	bool enabled = false;
};

struct HudMenuList {
	uint8_t count = 0;
	std::array<HudMenuEntry, kHudMenuEntries> entries{};
};

struct HudIngameMenu {
	HudMenuList menu_entries{};
};

struct HudUser {
	HudPlayerName user_name{};
	bool is_ingame = false;
	bool is_in_menu = false;
};

struct HudModel {
	uint32_t version = kHudModelVersion;
	uint64_t serial = 0;
	uint32_t tick = 0;
	uint8_t local_player = kHudNoPlayer;
	int8_t local_team = 0;
	uint8_t player_count = 0;
	std::array<HudPlayer, kHudMaxPlayers> players{};

	HudWeapon weapon;
	HudSoldier soldier;
	HudCapturePoints capture_points;
	HudTickets tickets;
	HudKillCounter kill_counter;
	HudMessages messages;
	HudMinimap minimap;
	HudSpawnPoints spawn_points;
	HudSpawnLogic spawn_logic;
	HudSquad squad;
	HudTeam team;
	HudKillCard kill_card;
	HudEndOfRound end_of_round;
	HudSpawnScreen spawn_screen;
	HudPreRound pre_round;
	HudDamageFeed damage;
	HudKit kit;
	HudCustomization customization;
	HudSettings settings;
	HudLevel level;
	HudIngameMenu ingame_menu;
	HudUser user;
};
static_assert(std::is_trivially_copyable_v<HudModel>);

enum class HudBindKind : uint8_t {
	Model,
	Client,
	Unused,
};

enum class HudWave : uint8_t {
	None,
	M2a,
	M2b,
	M3,
	M4,
	M5,
	M7,
	Screens,
};

struct HudBinding {
	std::string_view component;
	std::string_view source;
	HudBindKind kind = HudBindKind::Unused;
	HudWave wave = HudWave::None;
	std::string_view field;
};

inline constexpr HudBinding kHudBindings[] = {
	{ "UIWeaponCompData", "ActiveWeaponData", HudBindKind::Model, HudWave::M2a, "active_weapon_data" },
	{ "UIWeaponCompData", "ActiveWeaponCrosshair", HudBindKind::Model, HudWave::M2a, "active_weapon_crosshair" },
	{ "UIWeaponCompData", "ActiveWeaponAmmo", HudBindKind::Model, HudWave::M2a, "active_weapon_ammo" },
	{ "UIWeaponCompData", "ActiveWeaponClipsTotal", HudBindKind::Model, HudWave::M2a, "active_weapon_clips_total" },
	{ "UIWeaponCompData", "ActiveWeaponClipsLeft", HudBindKind::Model, HudWave::M2a, "active_weapon_clips_left" },
	{ "UIWeaponCompData", "ActiveWeaponReloadPercent", HudBindKind::Model, HudWave::M2a, "active_weapon_reload_percent" },
	{ "UIWeaponCompData", "ActiveWeaponAmmoInBag", HudBindKind::Model, HudWave::M2a, "active_weapon_ammo_in_bag" },
	{ "UIWeaponCompData", "ActiveWeaponClipCapacity", HudBindKind::Model, HudWave::M2a, "active_weapon_clip_capacity" },
	{ "UIWeaponCompData", "ActiveWeaponCriticalAmmo", HudBindKind::Model, HudWave::M2a, "active_weapon_critical_ammo" },
	{ "UIWeaponCompData", "ActiveWeaponIsReloading", HudBindKind::Model, HudWave::M2a, "active_weapon_is_reloading" },
	{ "UIWeaponCompData", "SecondaryWeaponAmmo", HudBindKind::Model, HudWave::M7, "secondary_weapon_ammo" },
	{ "UIWeaponCompData", "SecondaryWeaponClipsTotal", HudBindKind::Model, HudWave::M7, "secondary_weapon_clips_total" },
	{ "UIWeaponCompData", "SecondaryWeaponClipsLeft", HudBindKind::Model, HudWave::M7, "secondary_weapon_clips_left" },
	{ "UIWeaponCompData", "SecondaryWeaponReloadPercent", HudBindKind::Model, HudWave::M7, "secondary_weapon_reload_percent" },
	{ "UIWeaponCompData", "SecondaryWeaponAmmoInBag", HudBindKind::Model, HudWave::M7, "secondary_weapon_ammo_in_bag" },
	{ "UIWeaponCompData", "SecondaryWeaponClipCapacity", HudBindKind::Model, HudWave::M7, "secondary_weapon_clip_capacity" },
	{ "UIWeaponCompData", "SecondaryWeaponCriticalAmmo", HudBindKind::Model, HudWave::M7, "secondary_weapon_critical_ammo" },
	{ "UIWeaponCompData", "SecondaryWeaponIsReloading", HudBindKind::Model, HudWave::M7, "secondary_weapon_is_reloading" },
	{ "UIWeaponCompData", "SecondaryWeaponData", HudBindKind::Model, HudWave::M7, "secondary_weapon_data" },
	{ "UIWeaponCompData", "ShowHitindicator", HudBindKind::Model, HudWave::M2a, "show_hitindicator" },
	{ "UIWeaponCompData", "HitindicatorLevel", HudBindKind::Model, HudWave::M2a, "hitindicator_level" },
	{ "UIWeaponCompData", "ActiveWeaponAmmoTotal", HudBindKind::Model, HudWave::M2a, "active_weapon_ammo_total" },
	{ "UIWeaponCompData", "SecondaryWeaponAmmoTotal", HudBindKind::Model, HudWave::M7, "secondary_weapon_ammo_total" },
	{ "UIWeaponCompData", "ActiveWeaponName", HudBindKind::Model, HudWave::M2a, "active_weapon_name" },
	{ "UIWeaponCompData", "ActiveWeaponZoomed", HudBindKind::Model, HudWave::M2a, "active_weapon_zoomed" },
	{ "UIWeaponCompData", "ActiveWeaponIndex", HudBindKind::Model, HudWave::M2a, "active_weapon_index" },
	{ "UIWeaponCompData", "AimOnTeammate", HudBindKind::Model, HudWave::M2b, "aim_on_teammate" },
	{ "UIWeaponCompData", "AimOnEnemy", HudBindKind::Model, HudWave::M2b, "aim_on_enemy" },
	{ "UIWeaponCompData", "Dispersion", HudBindKind::Model, HudWave::M2a, "dispersion" },
	{ "UIWeaponCompData", "AbsoluteDispersion", HudBindKind::Model, HudWave::M2a, "absolute_dispersion" },
	{ "UIWeaponCompData", "Zoomed", HudBindKind::Model, HudWave::M2a, "zoomed" },
	{ "UIWeaponCompData", "ZoomTimer", HudBindKind::Model, HudWave::M2a, "zoom_timer" },
	{ "UIWeaponCompData", "Heat", HudBindKind::Model, HudWave::M5, "heat" },
	{ "UIWeaponCompData", "HeatPenalty", HudBindKind::Model, HudWave::M5, "heat_penalty" },
	{ "UIWeaponCompData", "Range", HudBindKind::Model, HudWave::M7, "range" },
	{ "UIWeaponCompData", "CrosshairScale", HudBindKind::Model, HudWave::M2a, "crosshair_scale" },
	{ "UIWeaponCompData", "FiringMode", HudBindKind::Model, HudWave::M2a, "firing_mode" },
	{ "UIWeaponCompData", "FiringModeTypes", HudBindKind::Model, HudWave::M2a, "firing_mode_types" },
	{ "UIWeaponCompData", "ActiveWeaponShowAmmo", HudBindKind::Model, HudWave::M2a, "active_weapon_show_ammo" },
	{ "UIWeaponCompData", "ShowCrosshair", HudBindKind::Model, HudWave::M2a, "show_crosshair" },
	{ "UIWeaponCompData", "CanFire", HudBindKind::Model, HudWave::M2a, "can_fire" },
	{ "UIWeaponCompData", "ShowTooltip", HudBindKind::Client, HudWave::None, "" },
	{ "UIWeaponCompData", "ReloadButtonId", HudBindKind::Client, HudWave::None, "" },
	{ "UIWeaponCompData", "ActiveWeaponProgress", HudBindKind::Model, HudWave::M7, "active_weapon_progress" },
	{ "UIWeaponCompData", "UnlockedWeaponReloading", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "CompassAngle", HudBindKind::Model, HudWave::M2a, "compass_angle" },
	{ "UIWeaponCompData", "WeaponPitchAngle", HudBindKind::Model, HudWave::M5, "weapon_pitch_angle" },
	{ "UIWeaponCompData", "SystemTime", HudBindKind::Client, HudWave::None, "" },
	{ "UIWeaponCompData", "RangeMeter", HudBindKind::Model, HudWave::M5, "range_meter" },
	{ "UIWeaponCompData", "GPSLatitudePosition", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "GPSLongitudePosition", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "TimeToLive", HudBindKind::Model, HudWave::M7, "time_to_live" },
	{ "UIWeaponCompData", "CrosshairDefaultInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "CrosshairCircleInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "CrosshairGrenadeInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "CrosshairGrenadeHideZoomInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "CrosshairDotInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "AATrackingLockingInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "TrackingLockingInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "JavelinInitialized", HudBindKind::Unused, HudWave::None, "" },
	{ "UIWeaponCompData", "ProjectileTypes", HudBindKind::Model, HudWave::M7, "projectile_types" },
	{ "UIWeaponCompData", "ProjectileIndex", HudBindKind::Model, HudWave::M7, "projectile_index" },
	{ "UIWeaponCompData", "HasAlternativeProjectiles", HudBindKind::Model, HudWave::M7, "has_alternative_projectiles" },
	{ "UISoldierCompData", "IsAlive", HudBindKind::Model, HudWave::M2b, "is_alive" },
	{ "UISoldierCompData", "Supressing", HudBindKind::Model, HudWave::M7, "supressing" },
	{ "UISoldierCompData", "Pose", HudBindKind::Model, HudWave::M2a, "pose" },
	{ "UISoldierCompData", "Stamina", HudBindKind::Unused, HudWave::None, "" },
	{ "UISoldierCompData", "Health", HudBindKind::Model, HudWave::M2b, "health" },
	{ "UISoldierCompData", "ShowMedicRevive", HudBindKind::Model, HudWave::M2b, "show_medic_revive" },
	{ "UISoldierCompData", "CameraBlur", HudBindKind::Model, HudWave::M2b, "camera_blur" },
	{ "UISoldierCompData", "FirstPerson", HudBindKind::Model, HudWave::M2a, "first_person" },
	{ "UISoldierCompData", "CompassAngle", HudBindKind::Model, HudWave::M2a, "compass_angle" },
	{ "UISoldierCompData", "IsInteractiveManDown", HudBindKind::Model, HudWave::M2b, "is_interactive_man_down" },
	{ "UISoldierCompData", "HudState", HudBindKind::Model, HudWave::M2a, "hud_state" },
	{ "UISoldierCompData", "ShowHudAmmo", HudBindKind::Model, HudWave::M2a, "show_hud_ammo" },
	{ "UISoldierCompData", "ShowHudCompass", HudBindKind::Model, HudWave::M2a, "show_hud_compass" },
	{ "UISoldierCompData", "TimeUntilRevive", HudBindKind::Model, HudWave::M2b, "time_until_revive" },
	{ "UISoldierCompData", "TimeUntilDeath", HudBindKind::Model, HudWave::M2b, "time_until_death" },
	{ "UISoldierCompData", "IsBeeingRevived", HudBindKind::Model, HudWave::M2b, "is_beeing_revived" },
	{ "UICapturepointCompData", "ActiveCapturepoints", HudBindKind::Model, HudWave::M4, "active_capturepoints" },
	{ "UICapturepointCompData", "CaptureInProgress", HudBindKind::Model, HudWave::M4, "capture_in_progress" },
	{ "UICapturepointCompData", "CapturedByTeam", HudBindKind::Model, HudWave::M4, "captured_by_team" },
	{ "UICapturepointCompData", "Controlled", HudBindKind::Model, HudWave::M4, "controlled" },
	{ "UICapturepointCompData", "CaptureProgress", HudBindKind::Model, HudWave::M4, "capture_progress" },
	{ "UICapturepointCompData", "FriendlyCapturepoints", HudBindKind::Model, HudWave::M4, "friendly_capturepoints" },
	{ "UICapturepointCompData", "NeutralCapturepoints", HudBindKind::Model, HudWave::M4, "neutral_capturepoints" },
	{ "UICapturepointCompData", "EnemyCapturepoints", HudBindKind::Model, HudWave::M4, "enemy_capturepoints" },
	{ "UICapturepointCompData", "TotalCapturepoints", HudBindKind::Model, HudWave::M4, "total_capturepoints" },
	{ "UICapturepointCompData", "CapturepointsStatusData", HudBindKind::Model, HudWave::M4, "capturepoints_status_data" },
	{ "UICapturepointCompData", "MinimapCapturepointsStatusData", HudBindKind::Model, HudWave::M4, "minimap_capturepoints_status_data" },
	{ "UICapturepointCompData", "SpectateCapturepointsStatusData", HudBindKind::Unused, HudWave::None, "" },
	{ "UITicketCounterCompData", "FriendlyTeamTickets", HudBindKind::Model, HudWave::M4, "friendly_team_tickets" },
	{ "UITicketCounterCompData", "EnemyTeamTickets", HudBindKind::Model, HudWave::M4, "enemy_team_tickets" },
	{ "UITicketCounterCompData", "Team1Tickets", HudBindKind::Model, HudWave::M4, "team1_tickets" },
	{ "UITicketCounterCompData", "Team2Tickets", HudBindKind::Model, HudWave::M4, "team2_tickets" },
	{ "UITicketCounterCompData", "Team3Tickets", HudBindKind::Unused, HudWave::None, "" },
	{ "UITicketCounterCompData", "Team4Tickets", HudBindKind::Unused, HudWave::None, "" },
	{ "UITicketCounterCompData", "Team1MaxTickets", HudBindKind::Model, HudWave::M4, "team1_max_tickets" },
	{ "UITicketCounterCompData", "Team2MaxTickets", HudBindKind::Model, HudWave::M4, "team2_max_tickets" },
	{ "UITicketCounterCompData", "Team3MaxTickets", HudBindKind::Unused, HudWave::None, "" },
	{ "UITicketCounterCompData", "Team4MaxTickets", HudBindKind::Unused, HudWave::None, "" },
	{ "UITicketCounterCompData", "FriendlyTeamMaxTickets", HudBindKind::Model, HudWave::M4, "friendly_team_max_tickets" },
	{ "UITicketCounterCompData", "EnemyTeamMaxTickets", HudBindKind::Model, HudWave::M4, "enemy_team_max_tickets" },
	{ "UITicketCounterCompData", "GlobalMaxTickets", HudBindKind::Model, HudWave::M4, "global_max_tickets" },
	{ "UIKillCounterCompData", "Team1Kills", HudBindKind::Model, HudWave::M2b, "team1_kills" },
	{ "UIKillCounterCompData", "Team2Kills", HudBindKind::Model, HudWave::M2b, "team2_kills" },
	{ "UIKillCounterCompData", "Team3Kills", HudBindKind::Model, HudWave::M2b, "team3_kills" },
	{ "UIKillCounterCompData", "Team4Kills", HudBindKind::Model, HudWave::M2b, "team4_kills" },
	{ "UIKillCounterCompData", "Team5Kills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "Team6Kills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "Team7Kills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "Team8Kills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "TeamKillsList", HudBindKind::Model, HudWave::M2b, "team_kills_list" },
	{ "UIKillCounterCompData", "Team1MaxKills", HudBindKind::Model, HudWave::M2b, "team1_max_kills" },
	{ "UIKillCounterCompData", "Team2MaxKills", HudBindKind::Model, HudWave::M2b, "team2_max_kills" },
	{ "UIKillCounterCompData", "Team3MaxKills", HudBindKind::Model, HudWave::M2b, "team3_max_kills" },
	{ "UIKillCounterCompData", "Team4MaxKills", HudBindKind::Model, HudWave::M2b, "team4_max_kills" },
	{ "UIKillCounterCompData", "Team5MaxKills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "Team6MaxKills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "Team7MaxKills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "Team8MaxKills", HudBindKind::Unused, HudWave::None, "" },
	{ "UIKillCounterCompData", "PlayerTeamKills", HudBindKind::Model, HudWave::M2b, "player_team_kills" },
	{ "UIKillCounterCompData", "LeadingTeamKills", HudBindKind::Model, HudWave::M2b, "leading_team_kills" },
	{ "UIKillCounterCompData", "PlayerTeam", HudBindKind::Model, HudWave::M2b, "player_team" },
	{ "UIKillCounterCompData", "PlayerTeamPosition", HudBindKind::Model, HudWave::M2b, "player_team_position" },
	{ "UIKillCounterCompData", "LeadingByKills", HudBindKind::Model, HudWave::M2b, "leading_by_kills" },
	{ "UIKillCounterCompData", "EnemyTeamKills", HudBindKind::Model, HudWave::M2b, "enemy_team_kills" },
	{ "UIKillCounterCompData", "EnemyTeam1Kills", HudBindKind::Model, HudWave::M7, "enemy_team1_kills" },
	{ "UIKillCounterCompData", "EnemyTeam2Kills", HudBindKind::Model, HudWave::M7, "enemy_team2_kills" },
	{ "UIKillCounterCompData", "EnemyTeam3Kills", HudBindKind::Model, HudWave::M7, "enemy_team3_kills" },
	{ "UIMessageCompData", "KillMessage", HudBindKind::Model, HudWave::M2b, "kill_message" },
	{ "UIMessageCompData", "ChatMessage", HudBindKind::Model, HudWave::M3, "chat_message" },
	{ "UIMessageCompData", "ScoreMinorMessage", HudBindKind::Model, HudWave::M2b, "score_minor_message" },
	{ "UIMessageCompData", "ScoreMajorMessage", HudBindKind::Model, HudWave::M2b, "score_major_message" },
	{ "UIMessageCompData", "GameMessage", HudBindKind::Model, HudWave::M2b, "game_message" },
	{ "UIMessageCompData", "StaticMessage", HudBindKind::Model, HudWave::M2b, "static_message" },
	{ "UIMessageCompData", "OutOfBoundsMessage", HudBindKind::Model, HudWave::M2b, "out_of_bounds_message" },
	{ "UIMessageCompData", "KillScoreMessage", HudBindKind::Model, HudWave::M2b, "kill_score_message" },
	{ "UIMessageCompData", "LogLinesMessage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIMessageCompData", "SubtitleMessage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIMessageCompData", "TooltipMessage", HudBindKind::Client, HudWave::None, "" },
	{ "UIMessageCompData", "RewardMessage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIMessageCompData", "LocalKillMessage", HudBindKind::Model, HudWave::M2b, "local_kill_message" },
	{ "UIMessageCompData", "FailStateMessage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIMessageCompData", "ScoreMessage", HudBindKind::Model, HudWave::M2b, "score_message" },
	{ "UIMessageCompData", "ScoreAggregation", HudBindKind::Model, HudWave::M2b, "score_aggregation" },
	{ "UIMessageCompData", "ShowChatLog", HudBindKind::Client, HudWave::None, "" },
	{ "UIMessageCompData", "ToggleChatLog", HudBindKind::Client, HudWave::None, "" },
	{ "UIMessageCompData", "ChangeChatLogMode", HudBindKind::Client, HudWave::None, "" },
	{ "UIMessageCompData", "CurrentChatLogMode", HudBindKind::Client, HudWave::None, "" },
	{ "UIMessageCompData", "ServerAdminYellMessage", HudBindKind::Model, HudWave::M3, "server_admin_yell_message" },
	{ "UIMessageCompData", "GunMasterMessage", HudBindKind::Model, HudWave::M7, "gun_master_message" },
	{ "UIMinimapCompData", "IsSmall", HudBindKind::Client, HudWave::None, "" },
	{ "UIMinimapCompData", "Zoom", HudBindKind::Client, HudWave::None, "" },
	{ "UIMinimapCompData", "PlayerIsCenter", HudBindKind::Client, HudWave::None, "" },
	{ "UIMinimapCompData", "ShowCrosshairInCenter", HudBindKind::Client, HudWave::None, "" },
	{ "UIMinimapCompData", "AirRadarMaskEnabled", HudBindKind::Model, HudWave::M5, "air_radar_mask_enabled" },
	{ "UIMinimapCompData", "RadarSweepDegree", HudBindKind::Model, HudWave::M5, "radar_sweep_degree" },
	{ "UIMinimapCompData", "MinimapZoomWhenMapIsLarge", HudBindKind::Client, HudWave::None, "" },
	{ "UIMinimapCompData", "MinimapZoomWhenMapIsSmall", HudBindKind::Client, HudWave::None, "" },
	{ "UISpawnPointCompData", "SpawnPoints", HudBindKind::Model, HudWave::M2b, "spawn_points" },
	{ "UISpawnPointCompData", "SelectedSpawnType", HudBindKind::Client, HudWave::None, "" },
	{ "UISpawnPointCompData", "SelectedSpawnPoint", HudBindKind::Client, HudWave::None, "" },
	{ "UISpawnPointCompData", "SelectedSpawnListItem", HudBindKind::Client, HudWave::None, "" },
	{ "UISpawnPointCompData", "CanSpawn", HudBindKind::Model, HudWave::M2b, "can_spawn" },
	{ "UISpawnPointCompData", "HasValidSpawn", HudBindKind::Model, HudWave::M2b, "has_valid_spawn" },
	{ "UISpawnLogicCompData", "TimeToRespawn", HudBindKind::Model, HudWave::M2b, "time_to_respawn" },
	{ "UISpawnLogicCompData", "StartTimeToRespawn", HudBindKind::Model, HudWave::M2b, "start_time_to_respawn" },
	{ "UISpawnLogicCompData", "DisableSuicide", HudBindKind::Model, HudWave::M2b, "disable_suicide" },
	{ "UISpawnLogicCompData", "AutoDeployEnabled", HudBindKind::Unused, HudWave::None, "" },
	{ "UISquadCompData", "SquadMembers", HudBindKind::Model, HudWave::M7, "squad_members" },
	{ "UISquadCompData", "SquadName", HudBindKind::Model, HudWave::M7, "squad_name" },
	{ "UISquadCompData", "SquadPrivacy", HudBindKind::Unused, HudWave::None, "" },
	{ "UISquadCompData", "LocalPlayerIsSquadLeader", HudBindKind::Model, HudWave::M7, "local_player_is_squad_leader" },
	{ "UISquadCompData", "SquadLength", HudBindKind::Model, HudWave::M7, "squad_length" },
	{ "UISquadCompData", "LocalPlayerHasSquad", HudBindKind::Model, HudWave::M7, "local_player_has_squad" },
	{ "UISquadCompData", "ButtonLayout", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SelectedPlayer", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "DisableLeaveSquad", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SquadListSelected", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "ActiveSquadBoosters", HudBindKind::Unused, HudWave::None, "" },
	{ "UISquadCompData", "AvailableSquads", HudBindKind::Model, HudWave::M7, "available_squads" },
	{ "UISquadCompData", "InviteList", HudBindKind::Unused, HudWave::None, "" },
	{ "UISquadCompData", "VoipSetting", HudBindKind::Unused, HudWave::None, "" },
	{ "UISquadCompData", "SelectedSquad", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SquadPrivacyEnabled", HudBindKind::Unused, HudWave::None, "" },
	{ "UISquadCompData", "MenuList", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SetValue", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "ButtonMode", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "JoinButton", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SquadIsEmpty", HudBindKind::Model, HudWave::M7, "squad_is_empty" },
	{ "UISquadCompData", "LocalSquadMembers", HudBindKind::Model, HudWave::M7, "local_squad_members" },
	{ "UISquadCompData", "SquadLeaderMessageShown", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SquadLeaderMessageVisibility", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "BackgroundMode", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SelectedSquadIndex", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "LocalSquadSelected", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "SquadButton", HudBindKind::Client, HudWave::None, "" },
	{ "UISquadCompData", "LeaderInActiveSquad", HudBindKind::Model, HudWave::M7, "leader_in_active_squad" },
	{ "UISquadCompData", "KickButton", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "ActiveTeamsData", HudBindKind::Model, HudWave::M2b, "active_teams_data" },
	{ "UITeamCompData", "TopPlayer1Name", HudBindKind::Model, HudWave::M2b, "top_player1_name" },
	{ "UITeamCompData", "TopPlayer2Name", HudBindKind::Model, HudWave::M2b, "top_player2_name" },
	{ "UITeamCompData", "TopPlayer3Name", HudBindKind::Model, HudWave::M2b, "top_player3_name" },
	{ "UITeamCompData", "FriendlyTeamPlayers", HudBindKind::Model, HudWave::M2b, "friendly_team_players" },
	{ "UITeamCompData", "EnemyTeam1Players", HudBindKind::Model, HudWave::M2b, "enemy_team1_players" },
	{ "UITeamCompData", "EnemyTeam2Players", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "EnemyTeam3Players", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "FriendlyTeamIsWinner", HudBindKind::Model, HudWave::M2b, "friendly_team_is_winner" },
	{ "UITeamCompData", "EnemyTeam1IsWinner", HudBindKind::Model, HudWave::M2b, "enemy_team1_is_winner" },
	{ "UITeamCompData", "EnemyTeam2IsWinner", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "EnemyTeam3IsWinner", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "Team1Name", HudBindKind::Model, HudWave::M2b, "team1_name" },
	{ "UITeamCompData", "Team2Name", HudBindKind::Model, HudWave::M2b, "team2_name" },
	{ "UITeamCompData", "Team3Name", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "Team4Name", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "NeutralPlayers", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "FriendlyTeamName", HudBindKind::Model, HudWave::M2b, "friendly_team_name" },
	{ "UITeamCompData", "EnemyTeam1Name", HudBindKind::Model, HudWave::M2b, "enemy_team1_name" },
	{ "UITeamCompData", "EnemyTeam2Name", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "EnemyTeam3Name", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "FriendlyTeam", HudBindKind::Model, HudWave::M2b, "friendly_team" },
	{ "UITeamCompData", "EnemyTeam1", HudBindKind::Model, HudWave::M2b, "enemy_team1" },
	{ "UITeamCompData", "EnemyTeam2", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "EnemyTeam3", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "Team1Players", HudBindKind::Model, HudWave::M2b, "team1_players" },
	{ "UITeamCompData", "Team2Players", HudBindKind::Model, HudWave::M2b, "team2_players" },
	{ "UITeamCompData", "Team3Players", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "Team4Players", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "CurrentSelectedPlayer", HudBindKind::Client, HudWave::None, "" },
	{ "UITeamCompData", "ScoreboardEnabled", HudBindKind::Model, HudWave::M2b, "scoreboard_enabled" },
	{ "UITeamCompData", "DefaultScoreboardIndex", HudBindKind::Client, HudWave::None, "" },
	{ "UITeamCompData", "ButtonLayout", HudBindKind::Client, HudWave::None, "" },
	{ "UITeamCompData", "Spectators", HudBindKind::Unused, HudWave::None, "" },
	{ "UITeamCompData", "SortedPlayers", HudBindKind::Model, HudWave::M2b, "sorted_players" },
	{ "UIKillCardCompData", "KillerName", HudBindKind::Model, HudWave::Screens, "killer_name" },
	{ "UIKillCardCompData", "KillerRank", HudBindKind::Model, HudWave::Screens, "killer_rank" },
	{ "UIKillCardCompData", "KillerHealth", HudBindKind::Model, HudWave::Screens, "killer_health" },
	{ "UIKillCardCompData", "KillerKit", HudBindKind::Model, HudWave::Screens, "killer_kit" },
	{ "UIKillCardCompData", "KillerScore", HudBindKind::Model, HudWave::Screens, "killer_score" },
	{ "UIKillCardCompData", "KillerWeaponName", HudBindKind::Model, HudWave::Screens, "killer_weapon_name" },
	{ "UIKillCardCompData", "KillerWeaponIcon", HudBindKind::Model, HudWave::Screens, "killer_weapon_icon" },
	{ "UIKillCardCompData", "isSuicide", HudBindKind::Model, HudWave::Screens, "is_suicide" },
	{ "UIKillCardCompData", "KillerPlayerKills", HudBindKind::Model, HudWave::Screens, "killer_player_kills" },
	{ "UIKillCardCompData", "PlayerKillerKills", HudBindKind::Model, HudWave::Screens, "player_killer_kills" },
	{ "UIKillCardCompData", "KillerWeaponSpec1Id", HudBindKind::Model, HudWave::Screens, "killer_weapon_spec1_id" },
	{ "UIKillCardCompData", "KillerWeaponSpec1Name", HudBindKind::Model, HudWave::Screens, "killer_weapon_spec1_name" },
	{ "UIKillCardCompData", "KillerWeaponSpec2Id", HudBindKind::Model, HudWave::Screens, "killer_weapon_spec2_id" },
	{ "UIKillCardCompData", "KillerWeaponSpec2Name", HudBindKind::Model, HudWave::Screens, "killer_weapon_spec2_name" },
	{ "UIKillCardCompData", "KillerWeaponSpec3Id", HudBindKind::Model, HudWave::Screens, "killer_weapon_spec3_id" },
	{ "UIKillCardCompData", "KillerWeaponSpec3Name", HudBindKind::Model, HudWave::Screens, "killer_weapon_spec3_name" },
	{ "UIKillCardCompData", "KillerWeaponType", HudBindKind::Model, HudWave::Screens, "killer_weapon_type" },
	{ "UIKillCardCompData", "KillerVehicleSeat", HudBindKind::Model, HudWave::Screens, "killer_vehicle_seat" },
	{ "UIKillCardCompData", "KillerWeaponImagePath", HudBindKind::Model, HudWave::Screens, "killer_weapon_image_path" },
	{ "UIKillCardCompData", "KillerRankImagePath", HudBindKind::Model, HudWave::Screens, "killer_rank_image_path" },
	{ "UIKillCardCompData", "KillerWeaponServiceStars", HudBindKind::Model, HudWave::Screens,
			"killer_weapon_service_stars" },
	{ "UIKillCardCompData", "KillerKitServiceStars", HudBindKind::Model, HudWave::Screens, "killer_kit_service_stars" },
	{ "UIKillCardCompData", "KillerDogtagImagePath", HudBindKind::Model, HudWave::Screens, "killer_dogtag_image_path" },
	{ "UIKillCardCompData", "KillerBasicDogTag", HudBindKind::Model, HudWave::Screens, "killer_basic_dog_tag" },
	{ "UIKillCardCompData", "KillerAdvancedDogTag", HudBindKind::Model, HudWave::Screens, "killer_advanced_dog_tag" },
	{ "UIKillCardCompData", "isBadLuck", HudBindKind::Model, HudWave::Screens, "is_bad_luck" },
	{ "UIKillCardCompData", "IsPro", HudBindKind::Model, HudWave::Screens, "is_pro" },
	{ "UIKitCompData", "SelectedKit", HudBindKind::Model, HudWave::Screens, "selected_kit" },
	{ "UIKitCompData", "NumberOfKits", HudBindKind::Model, HudWave::Screens, "number_of_kits" },
	{ "UIKitCompData", "SelectedKitName", HudBindKind::Model, HudWave::Screens, "selected_kit_name" },
	{ "UIKitCompData", "SelectedVehicleName", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustomizeInfo", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedWeaponCategory", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedVehicle", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedVehicleCategory", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustRecievedStats", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "KitPrimaryWeapon", HudBindKind::Model, HudWave::Screens, "kit_primary_weapon" },
	{ "UICustomizationCompData", "KitSecondaryWeapon", HudBindKind::Model, HudWave::Screens, "kit_secondary_weapon" },
	{ "UICustomizationCompData", "KitGadget1", HudBindKind::Model, HudWave::Screens, "kit_gadget1" },
	{ "UICustomizationCompData", "KitGadget2", HudBindKind::Model, HudWave::Screens, "kit_gadget2" },
	{ "UICustomizationCompData", "KitSpecialization", HudBindKind::Model, HudWave::Screens, "kit_specialization" },
	{ "UICustomizationCompData", "SelectedKitProgression", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "WeaponAccessoryMain", HudBindKind::Model, HudWave::Screens, "weapon_accessory_main" },
	{ "UICustomizationCompData", "WeaponAccessory1", HudBindKind::Model, HudWave::Screens, "weapon_accessory1" },
	{ "UICustomizationCompData", "WeaponAccessory2", HudBindKind::Model, HudWave::Screens, "weapon_accessory2" },
	{ "UICustomizationCompData", "WeaponAccessory3", HudBindKind::Model, HudWave::Screens, "weapon_accessory3" },
	{ "UICustomizationCompData", "WeaponAccessory4", HudBindKind::Model, HudWave::Screens, "weapon_accessory4" },
	{ "UICustomizationCompData", "LandVehiclesData", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "LandVehicleProperty1", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "LandVehicleProperty2", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "LandVehicleProperty3", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehiclesData", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehiclePilotProperty1", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehiclePilotProperty2", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehiclePilotProperty3", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehicleGunnerProperty1", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehicleGunnerProperty2", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "AirVehicleGunnerProperty3", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAccessory1", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAccessory2", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAccessory3", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAccessory4", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAccessory5", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAccessory6", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustPrimaryWeapon", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSecondaryWeapon", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustGadgetOne", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustGadgetTwo", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSpecialization", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "SaveAccessoriesEnabled", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "SaveLoadoutEnabled", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "KitAppearance", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustSelectedAppearance", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustomizationFrontend", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "SpawnIsSelectingItem", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "SpawnSelectedSlot", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "KitGridData", HudBindKind::Model, HudWave::Screens, "kit_grid_data" },
	{ "UICustomizationCompData", "KitGridSelected", HudBindKind::Model, HudWave::Screens, "kit_grid_selected" },
	{ "UICustomizationCompData", "SaveAppearanceEnabled", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "SaveVehicleAccessoriesEnabled", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "CustomizationTeam", HudBindKind::Model, HudWave::Screens, "customization_team" },
	{ "UICustomizationCompData", "CustomizationTabs", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "DeployTabs", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "IsCustomizationUnseen", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "ButtonLayout", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustomizationHeader", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "CustomizationSubHeader", HudBindKind::Client, HudWave::None, "" },
	{ "UICustomizationCompData", "KitsData", HudBindKind::Model, HudWave::Screens, "kits_data" },
	{ "UICustomizationCompData", "HasGunnerSeat", HudBindKind::Unused, HudWave::None, "" },
	{ "UICustomizationCompData", "StoredOptionAppearanceForKit", HudBindKind::Unused, HudWave::None, "" },
	{ "UIPreRoundCompData", "CountLeft", HudBindKind::Model, HudWave::Screens, "count_left" },
	{ "UIPreRoundCompData", "MessageSID", HudBindKind::Client, HudWave::None, "" },
	{ "UIPreRoundCompData", "TitleSID", HudBindKind::Client, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "WinningTeam", HudBindKind::Model, HudWave::Screens, "winning_team" },
	{ "UIEndOfRoundCompData", "LocalPlayerInWinningTeam", HudBindKind::Model, HudWave::Screens,
			"local_player_in_winning_team" },
	{ "UIEndOfRoundCompData", "TimeUntilLevelChange", HudBindKind::Model, HudWave::Screens, "time_until_level_change" },
	{ "UIEndOfRoundCompData", "Unlocks", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "BucketUnlocks", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "RankUnlocks", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "StatUnlocks", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "Awards", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "BucketUnlockLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "RankUnlockLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "AssaultScore", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "EngineerScore", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "ReconScore", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "SupportScore", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "CombatScore", HudBindKind::Model, HudWave::Screens, "combat_score" },
	{ "UIEndOfRoundCompData", "AwardScore", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "UnlockScore", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "TotalScore", HudBindKind::Model, HudWave::Screens, "total_score" },
	{ "UIEndOfRoundCompData", "Kills", HudBindKind::Model, HudWave::Screens, "kills" },
	{ "UIEndOfRoundCompData", "Deaths", HudBindKind::Model, HudWave::Screens, "deaths" },
	{ "UIEndOfRoundCompData", "KillDeathRatio", HudBindKind::Model, HudWave::Screens, "kill_death_ratio" },
	{ "UIEndOfRoundCompData", "BestWeaponInfo", HudBindKind::Model, HudWave::Screens, "best_weapon_info" },
	{ "UIEndOfRoundCompData", "BestVehicleInfo", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "RoundDuration", HudBindKind::Model, HudWave::Screens, "round_duration" },
	{ "UIEndOfRoundCompData", "KillsPerMinute", HudBindKind::Model, HudWave::Screens, "kills_per_minute" },
	{ "UIEndOfRoundCompData", "LongestStreak", HudBindKind::Model, HudWave::Screens, "longest_streak" },
	{ "UIEndOfRoundCompData", "LongestNemesisStreak", HudBindKind::Model, HudWave::Screens, "longest_nemesis_streak" },
	{ "UIEndOfRoundCompData", "DogtagsTaken", HudBindKind::Model, HudWave::Screens, "dogtags_taken" },
	{ "UIEndOfRoundCompData", "LongestHeadshot", HudBindKind::Model, HudWave::Screens, "longest_headshot" },
	{ "UIEndOfRoundCompData", "TotalPlaytime", HudBindKind::Model, HudWave::Screens, "total_playtime" },
	{ "UIEndOfRoundCompData", "ScorePerMinute", HudBindKind::Model, HudWave::Screens, "score_per_minute" },
	{ "UIEndOfRoundCompData", "Name", HudBindKind::Model, HudWave::Screens, "name" },
	{ "UIEndOfRoundCompData", "RankName", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "CurrentRankImage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "NextRankImage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "RSPMessage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "RoundStartRankImage", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "TeamResults", HudBindKind::Model, HudWave::Screens, "team_results" },
	{ "UIEndOfRoundCompData", "PlayerTeamIndex", HudBindKind::Model, HudWave::Screens, "player_team_index" },
	{ "UIEndOfRoundCompData", "AssignmentUnlocks", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "HasKarkandLicense", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "ButtonLayout", HudBindKind::Client, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "EORTabsSetup", HudBindKind::Client, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "MatchStats", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "InMatch", HudBindKind::Model, HudWave::Screens, "in_match" },
	{ "UIEndOfRoundCompData", "LevelChangeTimerText", HudBindKind::Client, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "IsRanked", HudBindKind::Model, HudWave::Screens, "is_ranked" },
	{ "UIEndOfRoundCompData", "IsSpectator", HudBindKind::Model, HudWave::Screens, "is_spectator" },
	{ "UIEndOfRoundCompData", "ScoreMultiplier", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "VehicleUnlockLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "KitUnlockLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UIEndOfRoundCompData", "PlayerWonPositionData", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "CurrentGameMode", HudBindKind::Model, HudWave::Screens, "current_game_mode" },
	{ "UISettingsCompData", "Platform", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "Ps3EnterButton", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "MasterVolume", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MusicVolume", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "DialogueVolume", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "YourSoundSystem", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VoiceOverLanguage", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "Brightness", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "Contrast", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "Resolution", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "TextureQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ShadowQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "EffectsQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "AntiAliasing", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "AnisotropicFilter", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VSync", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "AmbientOcclusion", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VerticalLook", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VerticalFlight", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "Vibration", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "Sensitivity", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "Subtitles", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "HintSystem", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "AimAssist", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "ReloadHint", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "SoldierButtons", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "SoldierSticks", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "VehicleButtons", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "VehicleSticks", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "HeliButtons", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "HeliSticks", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "CurrentGameModeName", HudBindKind::Model, HudWave::Screens, "current_game_mode_name" },
	{ "UISettingsCompData", "FullscreenEnabled", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "OverallGraphicsQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "FullscreenScreen", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "FullscreenEnabledBool", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "SettingsChanged", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "SetOptionValue", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "SetOptionKey", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ScreenSafeAreaWidth", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ScreenSafeAreaHeight", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ZoomToggle", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MeshQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "TransparentShadows", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MotionBlur", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "UndergrowthQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "TerrainQuality", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "AntiAliasingDeferred", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "AntiAliasingPost", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "FieldOfView", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "KeyBindings", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VideoSettingsChanged", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ExitingOptions", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "KeyBindingsColumn", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "KeyBindingsRow", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "KeyBindingMode", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VerticalLand", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "CurrentGameModeClass", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "SelectedTab", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "StereoMode", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "Stereoscopy", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "StereoConvergence", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "MotionBlurEnabled", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MouseRawInput", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MouseSensitivity", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "LayoutHighlighted", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "LayoutAlternativeSelected", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "LayoutAlternativesSelected", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "CalibrationDone", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "ControllerIndex", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "JetButtons", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "JetSticks", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "DimmerEnabled", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ButtonLayout", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "SetKeyBindingMode", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "VendorAmd", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "MouseSensitivityVehicle", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ColorblindSupport", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "NetworkFrequencyFactor", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "MinimapType", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MinimapTextureOpacity", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MinimapZoomWhenMapIsLarge", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "MinimapZoomWhenMapIsSmall", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "ConsoleAntiAlias", HudBindKind::Unused, HudWave::None, "" },
	{ "UISettingsCompData", "ShowSelect", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "BreathToggle", HudBindKind::Client, HudWave::None, "" },
	{ "UISettingsCompData", "IsPC", HudBindKind::Model, HudWave::Screens, "is_pc" },
	{ "UISettingsCompData", "IsPS3JapaneseDD", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "BuiltLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "BuiltSPLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "BuiltMPLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "BuiltCoopLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "BuiltAnyMPLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "BuiltAnyCoopLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedLevelId", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedLevelName", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedLevelDescription", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedLevelImage", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedLevelCompletedDifficulty", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "QuickMatchGamemode", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "QuickMatchHardcore", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "QuickMatchMap", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "QuickMatchRegion", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "QuickMatchServerType", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "LoadMPLevelsLocally", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "GameModes", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedCoopLevel", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedCoopDifficulty", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "SelectedCoopLeaderboardMission", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "UnlockedSPLevels", HudBindKind::Unused, HudWave::None, "" },
	{ "UILevelCompData", "CurrentLevel", HudBindKind::Model, HudWave::Screens, "current_level" },
	{ "UIIngameMenuCompData", "MenuEntries", HudBindKind::Model, HudWave::Screens, "menu_entries" },
	{ "UIPopupCompData", "PopupTitle", HudBindKind::Client, HudWave::None, "" },
	{ "UIPopupCompData", "PopupText", HudBindKind::Client, HudWave::None, "" },
	{ "UIPopupCompData", "PopupType", HudBindKind::Client, HudWave::None, "" },
	{ "UIPopupCompData", "PopupButtons", HudBindKind::Client, HudWave::None, "" },
	{ "UIPopupCompData", "PopupIsVisible", HudBindKind::Client, HudWave::None, "" },
	{ "UIPopupCompData", "ShowDimmer", HudBindKind::Client, HudWave::None, "" },
	{ "UIPopupCompData", "PopupId", HudBindKind::Client, HudWave::None, "" },
	{ "UserComponentData", "UserName", HudBindKind::Model, HudWave::Screens, "user_name" },
	{ "UserComponentData", "NeedToPressStart", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "HasPressedStart", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "WidgetPath", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "IsIngame", HudBindKind::Model, HudWave::Screens, "is_ingame" },
	{ "UserComponentData", "NeedToShowStartupVideos", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "InsufficientSpace", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "ArrivingFromLauncher", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "IsOpenBeta", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "IngameMenuHighlightedRow", HudBindKind::Client, HudWave::None, "" },
	{ "UserComponentData", "StartSinglePlayerMenu", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "IsInMenu", HudBindKind::Model, HudWave::Screens, "is_in_menu" },
	{ "UserComponentData", "AllowIdleMovie", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "ShowXPack1Obsolete", HudBindKind::Unused, HudWave::None, "" },
	{ "UserComponentData", "UserGeneratedContentEnabled", HudBindKind::Unused, HudWave::None, "" },
};

inline constexpr std::span<const HudBinding> hud_bindings() {
	return kHudBindings;
}

inline constexpr std::string_view kHudBoundComponents[] = {
	"UIWeaponCompData",
	"UISoldierCompData",
	"UICapturepointCompData",
	"UITicketCounterCompData",
	"UIKillCounterCompData",
	"UIMessageCompData",
	"UIMinimapCompData",
	"UISpawnPointCompData",
	"UISpawnLogicCompData",
	"UISquadCompData",
	"UITeamCompData",
	"UIKillCardCompData",
	"UIKitCompData",
	"UICustomizationCompData",
	"UIPreRoundCompData",
	"UIEndOfRoundCompData",
	"UISettingsCompData",
	"UILevelCompData",
	"UIIngameMenuCompData",
	"UIPopupCompData",
	"UserComponentData",
};

constexpr size_t hud_binding_count(std::string_view p_component) {
	size_t n = 0;
	for (const HudBinding &b : kHudBindings) {
		n += b.component == p_component ? 1 : 0;
	}
	return n;
}

constexpr size_t hud_binding_count(HudBindKind p_kind) {
	size_t n = 0;
	for (const HudBinding &b : kHudBindings) {
		n += b.kind == p_kind ? 1 : 0;
	}
	return n;
}

constexpr size_t hud_snake_case(std::string_view p_source, char *r_out, size_t p_capacity) {
	const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
	const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
	const auto digit = [](char c) { return c >= '0' && c <= '9'; };
	size_t n = 0;
	for (size_t i = 0; i < p_source.size(); ++i) {
		const char c = p_source[i];
		const bool boundary = i > 0 && upper(c) &&
				(lower(p_source[i - 1]) || digit(p_source[i - 1]) || (upper(p_source[i - 1]) && i + 1 < p_source.size() && lower(p_source[i + 1])));
		if (boundary) {
			if (n >= p_capacity) {
				return 0;
			}
			r_out[n++] = '_';
		}
		if (n >= p_capacity) {
			return 0;
		}
		r_out[n++] = upper(c) ? char(c - 'A' + 'a') : c;
	}
	return n;
}

struct HudBindingReport {
	size_t rows = 0;
	size_t matched = 0;
	std::array<size_t, 3> kinds{};
	std::vector<std::string> not_in_install;
	std::vector<std::string> not_in_rows;
	std::vector<std::string> unbound_components;
	size_t unbound_sources = 0;
	bool ok() const { return not_in_install.empty() && not_in_rows.empty(); }
};
HudBindingReport check_hud_bindings(const HudSourceTable &p_table);

}

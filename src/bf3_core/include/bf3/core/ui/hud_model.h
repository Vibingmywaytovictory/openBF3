#pragma once
// HudModel: the HUD's data model, the seam between the simulation and the HUD screens (plan §3.8 "Our sim/HUD bridge
// exposes the same names", D16; research/topics/gap-r1-ui-hud-minimap-localisation.md §5; docs/design/
// ui-minimap-icons.md §7). Godot-free plain data: the client fills one HudModel per presented frame (M2a/M2b, from
// the PresentationFrame and the snapshots), the Godot HUD screens read it on the main thread.
//
// Names. Every struct is one BF3 UI component type and every field one of its DataSources, in snake_case of the
// source name and in the data's order (UIWeaponCompData.ActiveWeaponClipCapacity -> HudWeapon::
// active_weapon_clip_capacity). kHudBindings lists every DataSource of the 11 bound component types, each as Model (a
// field below), Client (state the HUD screen keeps itself: selections, button layouts, toggles, zoom) or Unused (not
// in openBF3's scope: persistence, VOIP, console, the Scaleform crosshair init handshakes, teams 3-8, ...), and the
// wave that fills it. The other 47 in-game components are not bound (design §7.3; bf3dump hud-sources --unbound lists
// their sources). ctest ui.hud.sources checks the rows against the install table in both directions.
//
// Text is never stored as text here except player names and chat: SIDs travel as their text_hash (ui/text_db.h, the
// HUD resolves them through the Translation), items as their unlock identifier (ui/hud_icons.h ItemIconIndex gives the
// _small icon), players as indices into HudModel::players. The model is trivially copyable and fixed size (like
// PresentationFrame) so a client thread can publish it under a short mutex.

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

// ---- caps (fixed size) ----------------------------------------------------------------------------------------------
inline constexpr size_t kHudMaxPlayers = 64; // plan §3.5: 64-player servers
inline constexpr size_t kHudMaxCapturePoints = 16; // Metro CQS 5, Caspian CQS 7 (flags + HQs)
inline constexpr size_t kHudMaxSpawnPoints = 64;
inline constexpr size_t kHudMaxSquads = 32; // UISquadCompData names Squad1..Squad32 (Alpha..Celeste)
inline constexpr size_t kHudSquadSize = 4; // BF3 squads hold 4
inline constexpr size_t kHudKillFeedSize = 8;
inline constexpr size_t kHudChatLines = 8;
inline constexpr size_t kHudNameBytes = 32; // UTF-8, NUL padded
inline constexpr size_t kHudTextBytes = 128; // UTF-8, NUL padded
inline constexpr uint8_t kHudNoPlayer = 0xFF;

using HudPlayerName = std::array<char, kHudNameBytes>;
using HudText = std::array<char, kHudTextBytes>;

// One weapon in a slot (ActiveWeaponData, SecondaryWeaponData).
struct HudWeaponSlot {
	uint32_t item_id = 0; // unlock identifier: ItemIconIndex::find -> the _small icon
	uint32_t name_sid_hash = 0; // text_hash(UIWeaponDescription.Name)
	int32_t weapon_class = 0; // WeaponDef class (wc556x45mmNATO, ...) as the resolver's enum index
	uint32_t fire_modes = 0; // bit per FiringModeTypes entry
};

// A scoreboard row; lists below refer to these by index.
struct HudPlayer {
	HudPlayerName name{};
	int8_t team = 0; // 0 neutral, 1, 2
	int8_t squad = 0; // 0 none, 1..32
	int8_t kit = 0; // 0 assault, 1 engineer, 2 support, 3 recon (UIHudIcon_Kit*)
	bool alive = false;
	bool local = false;
	bool bot = false;
	int16_t kills = 0;
	int16_t deaths = 0;
	int32_t score = 0;
	uint16_t ping_ms = 0;
};

struct HudPlayerList {
	uint8_t count = 0;
	std::array<uint8_t, kHudMaxPlayers> index{}; // into HudModel::players
};

struct HudCapturePoint {
	float x = 0.0f; // Frostbite world metres (minimap_project)
	float z = 0.0f;
	int8_t owner = 0; // 0 neutral, 1, 2
	int8_t attacker = 0; // the team taking it, 0 none
	bool hq = false;
	bool contested = false;
	float progress = 0.0f; // 0..1 towards the attacker
	uint32_t label_sid_hash = 0; // LocationNameSid (ID_H_US_A -> "A")
	uint32_t location_sid_hash = 0; // LocationTextSid (ID_H_MAP_MP_SUBWAY_CAFE)
};

struct HudCapturePointList {
	uint8_t count = 0;
	std::array<HudCapturePoint, kHudMaxCapturePoints> points{};
};

struct HudTeamKillList {
	uint8_t count = 0;
	std::array<int32_t, 4> kills{};
};

// One kill-feed line (KillMessage, LocalKillMessage). The HUD shows lines whose serial is newer than its own clock.
struct HudKill {
	uint32_t serial = 0; // increases per kill; 0 = empty slot
	uint8_t killer = kHudNoPlayer;
	uint8_t victim = kHudNoPlayer;
	uint32_t item_id = 0; // weapon or gadget unlock identifier (_small icon)
	uint32_t vehicle_item_hash = 0; // non-zero for a vehicle kill (VehicleHudData.VehicleItemHash)
	bool headshot = false;
	bool suicide = false;
	bool team_kill = false;
};

struct HudKillFeed {
	uint32_t newest_serial = 0;
	std::array<HudKill, kHudKillFeedSize> lines{}; // ring, newest at (newest_serial % size)
};

struct HudChatLine {
	uint32_t serial = 0;
	uint8_t player = kHudNoPlayer;
	uint8_t channel = 0; // 0 all, 1 team, 2 squad, 3 admin
	HudText text{};
};

struct HudTextFeed {
	uint32_t newest_serial = 0;
	std::array<HudChatLine, kHudChatLines> lines{};
};

// A game / out-of-bounds / admin message: a SID and an optional formatted argument line.
struct HudTextMessage {
	uint32_t serial = 0;
	uint32_t sid_hash = 0;
	float seconds = 0.0f; // display time
	HudText text{};
};

// A score popup (ScoreMessage, ScoreMinorMessage, ...): the scoring SID (ID_SCORE_KILL) and its points.
struct HudScoreEvent {
	uint32_t serial = 0;
	uint32_t sid_hash = 0;
	int32_t points = 0;
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
	uint8_t player = kHudNoPlayer; // SquadMate
};

struct HudSpawnPointList {
	uint8_t count = 0;
	std::array<HudSpawnPoint, kHudMaxSpawnPoints> points{};
};

struct HudSquadMemberList {
	uint8_t count = 0;
	std::array<uint8_t, kHudSquadSize> index{}; // into HudModel::players, the leader first
};

struct HudSquadEntry {
	int8_t squad = 0; // 1..32
	uint8_t members = 0;
	bool open = true;
};

struct HudSquadList {
	uint8_t count = 0;
	std::array<HudSquadEntry, kHudMaxSquads> squads{};
};

// ---- one struct per bound component type (field order = DataSources order; generated rows, design §7.2) ---------------

// UIWeaponCompData
struct HudWeapon {
	HudWeaponSlot active_weapon_data{}; // ActiveWeaponData (M2a)
	uint32_t active_weapon_crosshair = 0; // ActiveWeaponCrosshair (M2a)
	int32_t active_weapon_ammo = 0; // ActiveWeaponAmmo (M2a)
	int32_t active_weapon_clips_total = 0; // ActiveWeaponClipsTotal (M2a)
	int32_t active_weapon_clips_left = 0; // ActiveWeaponClipsLeft (M2a)
	float active_weapon_reload_percent = 0.0f; // ActiveWeaponReloadPercent (M2a)
	int32_t active_weapon_ammo_in_bag = 0; // ActiveWeaponAmmoInBag (M2a)
	int32_t active_weapon_clip_capacity = 0; // ActiveWeaponClipCapacity (M2a)
	bool active_weapon_critical_ammo = false; // ActiveWeaponCriticalAmmo (M2a)
	bool active_weapon_is_reloading = false; // ActiveWeaponIsReloading (M2a)
	int32_t secondary_weapon_ammo = 0; // SecondaryWeaponAmmo (M7)
	int32_t secondary_weapon_clips_total = 0; // SecondaryWeaponClipsTotal (M7)
	int32_t secondary_weapon_clips_left = 0; // SecondaryWeaponClipsLeft (M7)
	float secondary_weapon_reload_percent = 0.0f; // SecondaryWeaponReloadPercent (M7)
	int32_t secondary_weapon_ammo_in_bag = 0; // SecondaryWeaponAmmoInBag (M7)
	int32_t secondary_weapon_clip_capacity = 0; // SecondaryWeaponClipCapacity (M7)
	bool secondary_weapon_critical_ammo = false; // SecondaryWeaponCriticalAmmo (M7)
	bool secondary_weapon_is_reloading = false; // SecondaryWeaponIsReloading (M7)
	HudWeaponSlot secondary_weapon_data{}; // SecondaryWeaponData (M7)
	bool show_hitindicator = false; // ShowHitindicator (M2a)
	float hitindicator_level = 0.0f; // HitindicatorLevel (M2a)
	int32_t active_weapon_ammo_total = 0; // ActiveWeaponAmmoTotal (M2a)
	int32_t secondary_weapon_ammo_total = 0; // SecondaryWeaponAmmoTotal (M7)
	uint32_t active_weapon_name = 0; // ActiveWeaponName (M2a)
	bool active_weapon_zoomed = false; // ActiveWeaponZoomed (M2a)
	int32_t active_weapon_index = 0; // ActiveWeaponIndex (M2a)
	bool aim_on_teammate = false; // AimOnTeammate (M2b)
	bool aim_on_enemy = false; // AimOnEnemy (M2b)
	float dispersion = 0.0f; // Dispersion (M2a)
	float absolute_dispersion = 0.0f; // AbsoluteDispersion (M2a)
	bool zoomed = false; // Zoomed (M2a)
	float zoom_timer = 0.0f; // ZoomTimer (M2a)
	float heat = 0.0f; // Heat (M5)
	bool heat_penalty = false; // HeatPenalty (M5)
	float range = 0.0f; // Range (M7)
	float crosshair_scale = 0.0f; // CrosshairScale (M2a)
	int32_t firing_mode = 0; // FiringMode (M2a)
	uint32_t firing_mode_types = 0; // FiringModeTypes (M2a)
	bool active_weapon_show_ammo = false; // ActiveWeaponShowAmmo (M2a)
	bool show_crosshair = false; // ShowCrosshair (M2a)
	bool can_fire = false; // CanFire (M2a)
	float active_weapon_progress = 0.0f; // ActiveWeaponProgress (M7)
	float compass_angle = 0.0f; // CompassAngle (M2a)
	float weapon_pitch_angle = 0.0f; // WeaponPitchAngle (M5)
	float range_meter = 0.0f; // RangeMeter (M5)
	float time_to_live = 0.0f; // TimeToLive (M7)
	uint32_t projectile_types = 0; // ProjectileTypes (M7)
	int32_t projectile_index = 0; // ProjectileIndex (M7)
	bool has_alternative_projectiles = false; // HasAlternativeProjectiles (M7)
};

// UISoldierCompData
struct HudSoldier {
	bool is_alive = false; // IsAlive (M2b)
	float supressing = 0.0f; // Supressing (M7)
	int32_t pose = 0; // Pose (M2a)
	float health = 0.0f; // Health (M2b)
	bool show_medic_revive = false; // ShowMedicRevive (M2b)
	float camera_blur = 0.0f; // CameraBlur (M2b)
	bool first_person = false; // FirstPerson (M2a)
	float compass_angle = 0.0f; // CompassAngle (M2a)
	bool is_interactive_man_down = false; // IsInteractiveManDown (M2b)
	int32_t hud_state = 0; // HudState (M2a)
	bool show_hud_ammo = false; // ShowHudAmmo (M2a)
	bool show_hud_compass = false; // ShowHudCompass (M2a)
	float time_until_revive = 0.0f; // TimeUntilRevive (M2b)
	float time_until_death = 0.0f; // TimeUntilDeath (M2b)
	bool is_beeing_revived = false; // IsBeeingRevived (M2b)
};

// UICapturepointCompData
struct HudCapturePoints {
	int32_t active_capturepoints = 0; // ActiveCapturepoints (M4)
	bool capture_in_progress = false; // CaptureInProgress (M4)
	int32_t captured_by_team = 0; // CapturedByTeam (M4)
	bool controlled = false; // Controlled (M4)
	float capture_progress = 0.0f; // CaptureProgress (M4)
	int32_t friendly_capturepoints = 0; // FriendlyCapturepoints (M4)
	int32_t neutral_capturepoints = 0; // NeutralCapturepoints (M4)
	int32_t enemy_capturepoints = 0; // EnemyCapturepoints (M4)
	int32_t total_capturepoints = 0; // TotalCapturepoints (M4)
	HudCapturePointList capturepoints_status_data{}; // CapturepointsStatusData (M4)
	HudCapturePointList minimap_capturepoints_status_data{}; // MinimapCapturepointsStatusData (M4)
};

// UITicketCounterCompData
struct HudTickets {
	int32_t friendly_team_tickets = 0; // FriendlyTeamTickets (M4)
	int32_t enemy_team_tickets = 0; // EnemyTeamTickets (M4)
	int32_t team1_tickets = 0; // Team1Tickets (M4)
	int32_t team2_tickets = 0; // Team2Tickets (M4)
	int32_t team1_max_tickets = 0; // Team1MaxTickets (M4)
	int32_t team2_max_tickets = 0; // Team2MaxTickets (M4)
	int32_t friendly_team_max_tickets = 0; // FriendlyTeamMaxTickets (M4)
	int32_t enemy_team_max_tickets = 0; // EnemyTeamMaxTickets (M4)
	int32_t global_max_tickets = 0; // GlobalMaxTickets (M4)
};

// UIKillCounterCompData
struct HudKillCounter {
	int32_t team1_kills = 0; // Team1Kills (M2b)
	int32_t team2_kills = 0; // Team2Kills (M2b)
	int32_t team3_kills = 0; // Team3Kills (M2b)
	int32_t team4_kills = 0; // Team4Kills (M2b)
	HudTeamKillList team_kills_list{}; // TeamKillsList (M2b)
	int32_t team1_max_kills = 0; // Team1MaxKills (M2b)
	int32_t team2_max_kills = 0; // Team2MaxKills (M2b)
	int32_t team3_max_kills = 0; // Team3MaxKills (M2b)
	int32_t team4_max_kills = 0; // Team4MaxKills (M2b)
	int32_t player_team_kills = 0; // PlayerTeamKills (M2b)
	int32_t leading_team_kills = 0; // LeadingTeamKills (M2b)
	int32_t player_team = 0; // PlayerTeam (M2b)
	int32_t player_team_position = 0; // PlayerTeamPosition (M2b)
	int32_t leading_by_kills = 0; // LeadingByKills (M2b)
	int32_t enemy_team_kills = 0; // EnemyTeamKills (M2b)
	int32_t enemy_team1_kills = 0; // EnemyTeam1Kills (M7)
	int32_t enemy_team2_kills = 0; // EnemyTeam2Kills (M7)
	int32_t enemy_team3_kills = 0; // EnemyTeam3Kills (M7)
};

// UIMessageCompData
struct HudMessages {
	HudKillFeed kill_message{}; // KillMessage (M2b)
	HudTextFeed chat_message{}; // ChatMessage (M3)
	HudScoreEvent score_minor_message{}; // ScoreMinorMessage (M2b)
	HudScoreEvent score_major_message{}; // ScoreMajorMessage (M2b)
	HudTextMessage game_message{}; // GameMessage (M2b)
	HudTextMessage static_message{}; // StaticMessage (M2b)
	HudTextMessage out_of_bounds_message{}; // OutOfBoundsMessage (M2b)
	HudScoreEvent kill_score_message{}; // KillScoreMessage (M2b)
	HudKillFeed local_kill_message{}; // LocalKillMessage (M2b)
	HudScoreEvent score_message{}; // ScoreMessage (M2b)
	int32_t score_aggregation = 0; // ScoreAggregation (M2b)
	HudTextMessage server_admin_yell_message{}; // ServerAdminYellMessage (M3)
	HudTextMessage gun_master_message{}; // GunMasterMessage (M7)
};

// UIMinimapCompData
struct HudMinimap {
	bool air_radar_mask_enabled = false; // AirRadarMaskEnabled (M5)
	float radar_sweep_degree = 0.0f; // RadarSweepDegree (M5)
};

// UISpawnPointCompData
struct HudSpawnPoints {
	HudSpawnPointList spawn_points{}; // SpawnPoints (M2b)
	bool can_spawn = false; // CanSpawn (M2b)
	bool has_valid_spawn = false; // HasValidSpawn (M2b)
};

// UISpawnLogicCompData
struct HudSpawnLogic {
	float time_to_respawn = 0.0f; // TimeToRespawn (M2b)
	float start_time_to_respawn = 0.0f; // StartTimeToRespawn (M2b)
	bool disable_suicide = false; // DisableSuicide (M2b)
};

// UISquadCompData
struct HudSquad {
	HudSquadMemberList squad_members{}; // SquadMembers (M7)
	int32_t squad_name = 0; // SquadName (M7)
	bool local_player_is_squad_leader = false; // LocalPlayerIsSquadLeader (M7)
	int32_t squad_length = 0; // SquadLength (M7)
	bool local_player_has_squad = false; // LocalPlayerHasSquad (M7)
	HudSquadList available_squads{}; // AvailableSquads (M7)
	bool squad_is_empty = false; // SquadIsEmpty (M7)
	HudSquadMemberList local_squad_members{}; // LocalSquadMembers (M7)
	bool leader_in_active_squad = false; // LeaderInActiveSquad (M7)
};

// UITeamCompData
struct HudTeam {
	int32_t active_teams_data = 0; // ActiveTeamsData (M2b)
	HudPlayerName top_player1_name{}; // TopPlayer1Name (M2b)
	HudPlayerName top_player2_name{}; // TopPlayer2Name (M2b)
	HudPlayerName top_player3_name{}; // TopPlayer3Name (M2b)
	HudPlayerList friendly_team_players{}; // FriendlyTeamPlayers (M2b)
	HudPlayerList enemy_team1_players{}; // EnemyTeam1Players (M2b)
	bool friendly_team_is_winner = false; // FriendlyTeamIsWinner (M2b)
	bool enemy_team1_is_winner = false; // EnemyTeam1IsWinner (M2b)
	uint32_t team1_name = 0; // Team1Name (M2b)
	uint32_t team2_name = 0; // Team2Name (M2b)
	uint32_t friendly_team_name = 0; // FriendlyTeamName (M2b)
	uint32_t enemy_team1_name = 0; // EnemyTeam1Name (M2b)
	int32_t friendly_team = 0; // FriendlyTeam (M2b)
	int32_t enemy_team1 = 0; // EnemyTeam1 (M2b)
	HudPlayerList team1_players{}; // Team1Players (M2b)
	HudPlayerList team2_players{}; // Team2Players (M2b)
	bool scoreboard_enabled = false; // ScoreboardEnabled (M2b)
	HudPlayerList sorted_players{}; // SortedPlayers (M2b)
};

// ---- M2b W2.0 (docs/design/m2b.md §2.6 #16, §4; owner ui; contract: docs/design/m2b-w2.md) -------------------------
// The four M2b screens that no bound component type above carries: the kill card (UIKillCardCompData), the round end
// (EndOfRound), the pre-round (PreRound) and the spawn screen (the spawnscreenpc flow; TDM spawns are random, so it has
// no point choice). They are not kHudBindings rows (the 11 bound types are unchanged, ui.hud.sources stays as it is);
// host::fill_hud_tdm (bf3/host/hud_feed.h) fills them. Times are seconds left, computed by the feed from the server
// ticks it is given, so the screens never read a tick. Players are indices into HudModel::players, which the M2b
// feed fills by server slot (players[slot], kHudMaxPlayers == net::kMaxPlayerRows, pinned by m2b.contract2).
inline constexpr size_t kHudTopPlayers = 3; // EndOfRound's top players (TopPlayer1Name .. TopPlayer3Name)
inline constexpr size_t kHudKitCount = 4; // assault, engineer, support, recon (host::kKitCount)
inline constexpr size_t kHudSpawnKitItems = 5; // the kit bar's items per kit: weapon slots 0, 1, 2, 6, 7 (host::kHudSpawnSlots)

// The own death's kill card (4 s after the own death, with the death camera; m2b.md §4).
struct HudKillCard {
	uint32_t serial = 0; // increases per own death that made a card; 0 = never
	bool visible = false;
	uint8_t killer = kHudNoPlayer; // into players; kHudNoPlayer: no other soldier (a fall, a suicide)
	int8_t killer_kit = -1; // HudPlayer::kit of the killer's row (-1 unknown)
	bool headshot = false;
	bool suicide = false; // the killer was the victim (fall damage)
	uint32_t item_id = 0; // the weapon's unlock identifier (_small icon), 0 for a fall
	uint32_t weapon_name_sid_hash = 0; // text_hash of the weapon's name, 0 for a fall
	float distance_m = -1.0f; // own corpse to the killer's presented position at the death; < 0 unknown
	float killer_health = 0.0f; // DeathEvent::killer_health (hp, 0..100)
	float seconds_left = 0.0f; // until the card hides
};

// The round end (on MatchState RoundOver, until the restart).
struct HudEndOfRound {
	bool visible = false;
	int8_t winner = 0; // 0 none, 1, 2
	bool local_won = false;
	std::array<int32_t, 2> team_kills{}; // MatchState::kills (team 1, team 2)
	int32_t kill_limit = 0;
	std::array<uint8_t, kHudTopPlayers> top_players{ kHudNoPlayer, kHudNoPlayer, kHudNoPlayer }; // by score, into players
	float seconds_to_next_round = 0.0f; // MatchState::restart_tick; 0 while not scheduled
};

// The spawn screen (at join and after each own death once the kill card is gone; m2b.md §4).
struct HudSpawnScreen {
	bool visible = false;
	bool at_join = false; // no own life yet this round (no timer, no revive line)
	uint8_t selected_kit = 0; // the kit Deploy sends (the screen's selection, kept by the game: HudFeedInput::spawn_kit)
	bool can_deploy = false; // a SpawnRequest would be accepted: the server tick reached own_respawn_tick, phase PreRound / InRound
	float seconds_to_deploy = 0.0f; // until can_deploy (0 when it already is)
	bool revive_possible = false; // the own man-down window is open (own_revive_until)
	float seconds_revive_left = 0.0f;
	// The kit bar: per kit the unlock identifiers of slots 0, 1, 2, 6, 7 of the own team's loadout (0 = empty slot;
	// assault's slot 2 is the defibrillator with TDM kits).
	std::array<std::array<uint32_t, kHudSpawnKitItems>, kHudKitCount> kit_items{};
};

// The pre-round (MatchState PreRound: the countdown and the input freeze).
struct HudPreRound {
	bool visible = false;
	bool counting_down = false; // MatchState::countdown_end_tick set (else: waiting for players)
	float seconds_left = 0.0f; // until StartRound
	bool input_restricted = false; // MatchState::restrict_until_tick not reached: the freeze is on
};

// ---- the model ---------------------------------------------------------------------------------------------------------

struct HudModel {
	uint32_t version = kHudModelVersion;
	uint64_t serial = 0; // increases with every publish
	uint32_t tick = 0; // sim tick of the own state it was built from
	uint8_t local_player = kHudNoPlayer; // index into players
	int8_t local_team = 0;
	uint8_t player_count = 0;
	std::array<HudPlayer, kHudMaxPlayers> players{};

	HudWeapon weapon; // UIWeaponCompData
	HudSoldier soldier; // UISoldierCompData
	HudCapturePoints capture_points; // UICapturepointCompData
	HudTickets tickets; // UITicketCounterCompData
	HudKillCounter kill_counter; // UIKillCounterCompData
	HudMessages messages; // UIMessageCompData
	HudMinimap minimap; // UIMinimapCompData
	HudSpawnPoints spawn_points; // UISpawnPointCompData
	HudSpawnLogic spawn_logic; // UISpawnLogicCompData
	HudSquad squad; // UISquadCompData
	HudTeam team; // UITeamCompData
	HudKillCard kill_card; // M2b W2.0 (ui): the own death's kill card
	HudEndOfRound end_of_round; // M2b W2.0 (ui)
	HudSpawnScreen spawn_screen; // M2b W2.0 (ui)
	HudPreRound pre_round; // M2b W2.0 (ui)
};
static_assert(std::is_trivially_copyable_v<HudModel>);

// ---- bindings ------------------------------------------------------------------------------------------------------------

enum class HudBindKind : uint8_t {
	Model, // a HudModel field (`field`), filled by the client in `wave`
	Client, // the HUD screen's own state (selection, layout, toggles, zoom): no model field
	Unused, // not in openBF3's scope (design §7.3 lists the reasons)
};

// The milestone (plan §6) whose wave fills a Model field.
enum class HudWave : uint8_t {
	None,
	M2a, // infantry gunplay: ammo, crosshair, hit indicator, compass
	M2b, // Metro TDM with bots: health, death, kill feed, scoreboard, spawn screen, kill counter
	M3, // two machines: chat, admin messages
	M4, // Caspian Conquest: capture points, tickets
	M5, // air and sea: heat, radar, range meter
	M7, // breadth: UGL / secondary weapon, squads, gun master, suppression
};

struct HudBinding {
	std::string_view component; // UI*CompData type
	std::string_view source; // DataSources entry, exact spelling
	HudBindKind kind = HudBindKind::Unused;
	HudWave wave = HudWave::None;
	std::string_view field; // snake_case(source) for Model rows, else empty
};

// Every DataSource of the 11 bound component types, in (component, data) order. Generated from the install's
// ingamestatemp closure on 2026-10-15 (design §7.2) and checked against it by ctest ui.hud.sources.
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
};

inline constexpr std::span<const HudBinding> hud_bindings() {
	return kHudBindings;
}

// The 11 bound component types, in kHudBindings order.
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

// The UpperCamel source name in snake_case, as the Model rows' `field` spell it: a lower or digit followed by an
// upper starts a word, and so does the last upper of an upper run followed by a lower ("AATracking" -> "aa_tracking").
// Writes at most p_capacity bytes; returns the length (0 when it does not fit).
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

// The rows against an install table (bodies: the implementation wave, design §15).
struct HudBindingReport {
	size_t rows = 0; // kHudBindings rows checked
	size_t matched = 0; // rows whose (component, source) the install lists
	std::array<size_t, 3> kinds{}; // Model, Client, Unused
	std::vector<std::string> not_in_install; // "component.source" of rows the install does not list
	std::vector<std::string> not_in_rows; // "component.source" of the bound types' install sources without a row
	std::vector<std::string> unbound_components; // in-game components without rows (47 on retail)
	size_t unbound_sources = 0; // their DataSources
	bool ok() const { return not_in_install.empty() && not_in_rows.empty(); }
};
HudBindingReport check_hud_bindings(const HudSourceTable &p_table);

} // namespace bf3::core::ui

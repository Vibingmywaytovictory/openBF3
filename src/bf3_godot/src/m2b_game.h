#pragma once
// What the M2b session hands its parts (docs/design/m2b.md §2.6 #18, §4, §11 gx row; owner gx; W2.0 contract,
// docs/design/m2b-w2.md). Godot-free, so m2b.contract2 compiles it and ui's / cam's rows can build the same inputs.
//
// The M2b session is the M2a session (m2a_game.h: viewmodel, HUD, crowd, FX, audio, the same hooks and order) with the
// TDM rules on: the scene res://scenes/m2b_metro.tscn carries the metadata "openbf3_m2b" (or the command line has
// --m2b-check); gx's tagged lines `// M2b gx [<tag>]` in the M2a hooks of level_session_m2a.cpp then
//   load    set SharedLoadOptions::tdm (the logic graph, the TDM kits with the defibrillator, the immunity abort) for
//           the one load the server and the client share;
//   server  GameServerConfig::rules (mode Tdm, deploy_screen, kill_limit_override, preround_override_s) and
//           GameServerConfig::tdm_bots (TdmBotConfig::count = M2bOptions::bots) instead of the firefight bots;
//   frame   after the M2a parts of the frame: fill_hud_tdm (ui) after fill_hud with the same drained events,
//           Bf3TdmHud::set_model, Bf3DeathCamera::step (cam) while the own soldier is dead, the spawn screen's input
//           (kit keys, Enter / Deploy click -> ClientSession::send_spawn_request), Tab -> the scoreboard;
//   check   the M2b check (m2b_check.h) instead of the M2a one.
// ui's and cam's parts are built only when their body is in the tree (OPENBF3_M2B_HAVE_UI / _CAM from the files'
// presence, the "# M2b W2.0" block of bf3_godot's CMake), so gx links before they land.

#include "m2a_game.h"

#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <span>
#include <string>

namespace bf3::godot_side {

inline constexpr uint32_t kM2bPlayableDefaultBots = 31; // 16 v 16 with the player (m2b.md §12 J6)
// A SpawnRequest is refused silently (m2b.md §3.4: counted on the server only), so while the spawn screen can deploy
// and no Spawned came, the game sends the request again after this many server ticks [inferred].
inline constexpr uint32_t kM2bSpawnRequestRetryTicks = 30;

// The M2b user args (after "--"; play.cmd's defaults), parsed by gx's [args] line.
struct M2bOptions {
	bool deploy_screen = true; // --deploy-screen=0: M2a's automatic respawn with the KitSelect kit
	int kill_limit = 0; // --kill-limit=N (> 0 replaces the data's 100; MatchRulesConfig::kill_limit_override)
	float preround_s = -1.0f; // --preround=S (< 0: the data's 10 s; 0: none)
	uint32_t bots = kM2bPlayableDefaultBots; // --bots=N (TDM bots, capped by the free slots)
	std::string tdm_tuning_cfg; // config/tdm_tuning.cfg when present ("" = built-in defaults)
	std::string bot_tuning_cfg; // config/bot_tuning.cfg when present
};

// The loader inputs of the M2b parts' load steps (loader thread, after the M2a parts').
struct M2bLoadInputs {
	M2aLoadInputs m2a;
	M2bOptions options;
};

// One display frame as the M2b parts see it (main thread; valid for the step calls of this frame).
struct M2bFrame {
	const M2aFrame *m2a = nullptr; // the M2a parts' frame (events, others, camera)
	bool match_valid = false; // PresentationFrame::match_valid
	const net::MatchState *match = nullptr; // PresentationFrame::match (null before the first)
	std::span<const net::PlayerRow> players; // PresentationFrame::players[0, player_count)
	sim::Tick server_tick = 0; // PresentationFrame::newest_tick
	double now_s = 0.0; // the presentation clock (HudFeedInput::now_s)
	bool own_alive = false;
	bool own_unspawned = false; // PresentationFrame::own_unspawned
	bool spawn_screen = false; // the spawn screen is up: the kit keys and Enter go to it, the mouse is visible
	bool death_view = false; // Bf3DeathCamera is active: the 1p camera and viewmodel are hidden
};

} // namespace bf3::godot_side

// ---- M2b gx (as built: docs/design/m2b-gx.md) -------------------------------------------------------------------
// The Godot-free rules of the M2b session (m2b_game.cpp), so m2b.gx.unit tests them without Godot: the user args, the
// spawn screen's visibility and Deploy as the game routes input (the same rule as fill_hud_tdm's spawn_screen,
// hud_feed.h), the SpawnRequest resend, and the play check's rows from its tally (m2b_check.cpp gathers the tally).

#include "m2b_check.h" // M2bTrack

#include "bf3/core/result.h"

#include <vector>

namespace bf3::godot_side {

inline constexpr uint32_t kM2bMaxBots = 31; // 16 v 16 with the player (RoundNativeConfig::max_players 32)
inline constexpr float kM2bMaxPreroundSeconds = 600.0f;
inline constexpr int kM2bMaxKillLimit = 100000;

// The M2b user args as given ("" when absent; the session reads them with user_arg_value / has_user_arg).
struct M2bArgText {
	bool has_kill_limit = false, has_preround = false, has_bots = false, has_deploy_screen = false;
	std::string kill_limit, preround, bots, deploy_screen;
};
// Fills r_options from the args; a value out of range or not a number is an error (r_options keeps the default for
// it), never a clamp the user does not see.
core::Status parse_m2b_options(const M2bArgText &p_args, M2bOptions &r_options);

// The spawn screen is up (fill_hud_tdm's rule): the match is known, the own soldier is not alive (or was unspawned),
// no kill card is up, and the round is not over (the round end shows then).
struct M2bScreenInput {
	bool match_valid = false;
	net::MatchPhase phase = net::MatchPhase::Loading;
	bool own_alive = false;
	bool own_unspawned = false;
	bool kill_card_up = false;
};
bool m2b_spawn_screen_up(const M2bScreenInput &p_input);
// Deploy is enabled: the server's tick reached the own respawn tick (0 at join) in PreRound or InRound.
bool m2b_can_deploy(bool p_match_valid, const net::MatchState &p_match, sim::Tick p_server_tick, sim::Tick p_own_respawn_tick);

// Deploy: a press arms it, the request goes out while the spawn screen is up and Deploy is enabled, and again every
// kM2bSpawnRequestRetryTicks server ticks without a new life (the server refuses an early request silently).
struct M2bDeploy {
	bool armed = false; // Deploy pressed (or clicked) and no new life yet
	uint8_t kit = 0; // the kit of the armed request
	bool sent = false;
	sim::Tick sent_tick = 0; // server tick of the last request
	uint64_t requests = 0, resends = 0;
};
// One frame: true when a SpawnRequest {r_deploy.kit} is to be sent now.
bool m2b_deploy_step(M2bDeploy &r_deploy, bool p_screen_up, bool p_can_deploy, sim::Tick p_server_tick);
// A new own life arrived: the armed request is done.
void m2b_deploy_spawned(M2bDeploy &r_deploy);

// The play case (E9) as counted by m2b_check.cpp. "ui" rows read fill_hud_tdm's model (headless) and, in a window,
// Bf3TdmHud's records; "cam" rows Bf3DeathCamera's stats.
struct M2bPlayTally {
	double seconds = 0;
	bool windowed = false;
	// gx
	bool match_seen = false; // a MatchState arrived
	bool in_round_seen = false;
	bool join_screen_seen = false; // the spawn screen was up before the first own life of the round
	uint32_t own_deaths = 0; // own Death events (kOwnVictim)
	uint32_t deaths_checked = 0; // own deaths early enough for the screen to show (kKillCardSeconds + 4 s before the end), and not revived first
	uint32_t screens_after_death = 0; // of those, the spawn screen came up after the death
	uint32_t deploys = 0; // new lives after an armed SpawnRequest
	uint32_t deploy_kit_mismatch = 0; // a deployed life's kit != the requested kit
	uint32_t kits_deployed_mask = 0; // bit per deployed kit
	uint64_t spawn_requests = 0, spawn_request_resends = 0;
	bool round_over_seen = false; // MatchState RoundOver
	uint64_t deaths_received = 0; // every Death event drained
	// ui (fill_hud_tdm's model; the records in a window)
	uint64_t feed_lines = 0; // HudTdmFeedState::deaths_fed (model) / TdmHudRecords::kill_feed_lines_started (window)
	uint32_t scoreboard_samples = 0, scoreboard_mismatch = 0; // Tab held: the drawn team counts against MatchState
	uint32_t kill_cards_checked = 0, kill_cards_bad = 0; // own deaths by another soldier: the card's killer and weapon
	uint32_t spawn_screen_model_shown = 0; // the model's spawn screen became visible (window: the records' count)
	bool round_end_shown = false; // the model's (window: the records') round end on RoundOver
	uint64_t missing_lookups = 0; // fonts + icons + strings (window only)
	// cam
	uint64_t death_views = 0; // Bf3DeathCamera::stats().views
	uint64_t death_view_bad_inputs = 0;
};

enum class M2bRowState : uint8_t { Pass = 0, Fail = 1, Skip = 2 };
struct M2bPlayRow {
	std::string name;
	double value = 0.0;
	std::string rule;
	M2bRowState state = M2bRowState::Skip;
	M2bTrack track = M2bTrack::Gx;
	std::string skip_reason; // "waiting for ui", "no window"
};
// The rows of the play case: gx's always; ui's when p_have_ui (else skipped "waiting for ui"); cam's when p_have_cam.
// Rows that need the drawn screens (the lookups) are skipped without a window ("headless: nothing is drawn").
std::vector<M2bPlayRow> m2b_play_rows(const M2bPlayTally &p_tally, bool p_have_ui, bool p_have_cam);

} // namespace bf3::godot_side

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

#pragma once
// The level session's M2b state (docs/design/m2b.md §2.6 #18, §11 gx row; owner gx; W2.0 contract,
// docs/design/m2b-w2.md). M2bGame hangs off the M2a session (M2aGame::m2b, null outside an M2b session) and holds the
// M2b parts gx builds: ui's Bf3TdmHud (hud_tdm.h) and its feed state, cam's Bf3DeathCamera (death_camera.h), the
// spawn-screen input state and the M2b check. gx's code reaches it from tagged lines `// M2b gx [<tag>]` in the M2a
// hooks (level_session_m2a.cpp); those functions are gx's own (no other track calls them), declared in gx's files.
// Godot-free header (forward declarations only), so m2b.contract2 compiles it.

#include "death_camera.h"
#include "hud_tdm.h"
#include "m2b_check.h"
#include "m2b_game.h"

#include "bf3/host/hud_feed.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

struct M2bGame {
	// ---- args (gx's [args] line) ----
	bool enabled = false;
	bool playable = false; // the scene's "openbf3_m2b" metadata (not a check run)
	M2bOptions options;
	std::vector<M2bCase> cases;
	M2bCheckOptions check_options;
	bool checks() const { return !cases.empty(); }

	// ---- ui (OPENBF3_M2B_HAVE_UI) ----
	std::unique_ptr<Bf3TdmHud, void (*)(Bf3TdmHud *)> tdm_hud{ nullptr, nullptr };
	bf3::host::HudTdmFeedState tdm_state;
	bf3::host::HudKillCardSource kill_card_source; // set in the frame that drains the own Death event

	// ---- cam (OPENBF3_M2B_HAVE_CAM) ----
	std::unique_ptr<Bf3DeathCamera, void (*)(Bf3DeathCamera *)> death_camera{ nullptr, nullptr };

	// ---- the spawn screen and the scoreboard (main thread) ----
	uint8_t spawn_kit = 0; // the selected kit (keys, Bf3TdmHud::take_kit_click)
	bool scoreboard_held = false; // Tab
	bool spawned_this_round = false; // HudFeedInput::spawned_this_round
	uint64_t spawn_requests_sent = 0;
	bf3::sim::Tick last_request_tick = 0; // server tick of the last SpawnRequest (kM2bSpawnRequestRetryTicks)
	uint32_t own_life_seen = 0;

	// ---- per frame ----
	M2bFrame frame;
	double ms_tdm_hud = 0, ms_death_camera = 0; // wall milliseconds of the M2b parts this frame (§8: <= 0.3 ms)

	// ---- the check ----
	std::unique_ptr<M2bCheck, void (*)(M2bCheck *)> check{ nullptr, nullptr }; // a deleter of gx's (M2bCheck's body is gx's)
	bool finished = false;
};

} // namespace bf3::godot_side

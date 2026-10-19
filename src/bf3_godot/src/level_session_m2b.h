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

namespace godot { // M2b gx
class Camera3D; // M2b gx
class Label; // M2b gx
} // namespace godot // M2b gx

namespace bf3::host { // M2b gx
struct GameServerConfig; // M2b gx
struct SharedLoadOptions; // M2b gx
class ClientSession; // M2b gx
struct PresentationFrame; // M2b gx
} // namespace bf3::host // M2b gx

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

	// ---- M2b gx (as built: docs/design/m2b-gx.md) ----
	bool built = false; // the Godot side (m2b_gx_frame's first call)
	bool windowed = false;
	godot::Label *status_label = nullptr; // the spawn screen's stand-in while ui has not landed (OPENBF3_M2B_HAVE_UI = 0)
	M2bDeploy deploy; // Deploy pressed -> SpawnRequest (resent every kM2bSpawnRequestRetryTicks)
	bool spawn_screen = false; // m2b_spawn_screen_up this frame (input routing; the drawn screen is ui's)
	bool can_deploy = false;
	bool round_end = false; // MatchState RoundOver
	net::MatchPhase last_phase = net::MatchPhase::Loading;
	bool mouse_released = false; // the game released the mouse for the spawn screen / the round end
	uint8_t own_slot = 0xFF; // MatchInfo's slot once the match is known (spawned or not)
	uint8_t own_team = 0; // PresentationFrame::own_team, else the own player row's
	// The own death (the frame its Death event drained): the kill card's and the death view's timer.
	double own_death_s = -1.0; // presentation clock; < 0 none
	uint8_t own_killer = 0xFF;
	uint32_t own_deaths = 0;
	uint64_t deaths_received = 0;
	bool life_revived = false; // the current own life began as a revive (PresentKind::Revive for the own slot)
	uint32_t deploys = 0, deploy_kit_mismatch = 0;
	// The death view's last camera (kept while dead, so the spawn screen does not jump back to the corpse's eye).
	bool view_valid = false;
	float view_basis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // columns x, y, z of the Godot basis
	float view_origin[3] = { 0, 0, 0 };
	bool join_view_placed = false; // the camera behind the join spawn screen (no own soldier yet)
	uint64_t ui_build_failures = 0;
	float corpse_seconds = 0.0f; // SoldierDef::time_for_corpse (the server config's level; 0: anim::kCrowdCorpseSeconds)
	double revive_seen_s = -1.0; // a PresentKind::Revive of the own slot was drained then
	uint8_t kit_check_kit = 0xFF; // a deploy's requested kit, checked against PresentationFrame::own_kit
	double kit_check_until_s = 0.0;
	bool death_view_pending = false; // the own death drained in [presented]: [frame] begins the death view
	bool death_view_end = false; // a new life: [frame] ends it
	bool was_alive = false; // r_game.alive of the last [presented] (a revive keeps the life number)
	bool deploy_click = false; // Bf3TdmHud's Deploy click, taken by the next [presented]
};

// ---- M2b gx: the hooks (docs/design/m2b-gx.md §2) -----------------------------------------------------------------
// Called from one tagged line `// M2b gx [<tag>]` each in level_session_m2a.cpp's M2a hooks (and [done] in
// level_session.cpp's try_done); every one returns at once when r_game.m2b is null (not an M2b session). The bodies
// are in level_session_m2b.cpp (Godot side).
class InputMapper;
// [args], main thread, end of m2a_parse_args: makes r_game.m2b when the scene carries "openbf3_m2b" or the command
// line has --m2b-check; parses M2bOptions (--kill-limit, --preround, --bots, --deploy-screen) and the check; sets the
// bot count (31 by default) and the check run's quit guard; r_error gets the first bad argument.
void m2b_gx_args(M2aGame &r_game, bool p_scene_m2b, InputMapper &r_mapper, uint32_t &r_bots, double &r_quit_after, std::string &r_error);
// [load], loader thread: SharedLoadOptions::tdm (the logic graph, the TDM kits, the immunity abort) and tdm_tuning.cfg.
void m2b_gx_load(const M2aGame &p_game, bf3::host::SharedLoadOptions &r_options);
// [server], loader thread, end of m2a_server_config: the TDM rules (deploy screen, kill limit, pre-round) and the TDM
// bots instead of the firefight bots; the death view's collision world.
void m2b_gx_server(M2aGame &r_game, bf3::host::GameServerConfig &r_config, uint32_t p_bots);
// [presented], main thread, every frame from the session start (m2a_presented, before its K / T step): the own slot
// and team, the own death and new lives from the drained events, the spawn screen's state and keys, Deploy.
void m2b_gx_presented(M2aGame &r_game, bf3::host::ClientSession &r_client, const bf3::host::PresentationFrame &p_frame, InputMapper &r_mapper,
		uint64_t p_now_ns);
// [frame], main thread, after Done (m2a_frame, after the HUD): builds the M2b parts once, feeds ui's screens and cam's
// death view, the mouse mode, and steps the M2b check. True only on the frame the check finished the run (the caller
// returns at once: finish() has shut the session down).
bool m2b_gx_frame(Bf3LevelSession &r_session, M2aGame &r_game, const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns, double p_delta,
		godot::Camera3D *p_camera, bool p_free_camera, bool p_headless);
// [shutdown], main thread (m2a_shutdown): the check, ui's screens, cam's view.
void m2b_gx_shutdown(M2aGame &r_game);

} // namespace bf3::godot_side

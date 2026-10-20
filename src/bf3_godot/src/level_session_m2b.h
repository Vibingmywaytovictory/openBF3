#pragma once

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

namespace godot {
class Camera3D;
class Label;
}

namespace bf3::host {
struct GameServerConfig;
struct SharedLoadOptions;
class ClientSession;
struct PresentationFrame;
}

namespace bf3::godot_side {

struct M2bGame {

	bool enabled = false;
	bool playable = false;
	M2bOptions options;
	std::vector<M2bCase> cases;
	M2bCheckOptions check_options;
	bool checks() const { return !cases.empty(); }

	std::unique_ptr<Bf3TdmHud, void (*)(Bf3TdmHud *)> tdm_hud{ nullptr, nullptr };
	bf3::host::HudTdmFeedState tdm_state;
	bf3::host::HudKillCardSource kill_card_source;

	std::unique_ptr<Bf3DeathCamera, void (*)(Bf3DeathCamera *)> death_camera{ nullptr, nullptr };

	uint8_t spawn_kit = 0;
	bool scoreboard_held = false;
	bool spawned_this_round = false;
	uint64_t spawn_requests_sent = 0;
	bf3::sim::Tick last_request_tick = 0;
	uint32_t own_life_seen = 0;

	M2bFrame frame;
	double ms_tdm_hud = 0, ms_death_camera = 0;

	std::unique_ptr<M2bCheck, void (*)(M2bCheck *)> check{ nullptr, nullptr };
	bool finished = false;

	bool built = false;
	bool windowed = false;
	godot::Label *status_label = nullptr;
	M2bDeploy deploy;
	bool spawn_screen = false;
	bool can_deploy = false;
	bool round_end = false;
	net::MatchPhase last_phase = net::MatchPhase::Loading;
	bool mouse_released = false;
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0;

	double own_death_s = -1.0;
	uint8_t own_killer = 0xFF;
	uint32_t own_deaths = 0;
	uint64_t deaths_received = 0;
	bool life_revived = false;
	uint32_t deploys = 0, deploy_kit_mismatch = 0;

	bool view_valid = false;
	float view_basis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	float view_origin[3] = { 0, 0, 0 };
	bool join_view_placed = false;
	uint64_t ui_build_failures = 0;
	float corpse_seconds = 0.0f;
	double revive_seen_s = -1.0;
	uint8_t kit_check_kit = 0xFF;
	double kit_check_until_s = 0.0;
	bool death_view_pending = false;
	bool death_view_end = false;
	bool was_alive = false;
	bool deploy_click = false;
};

class InputMapper;

void m2b_gx_args(M2aGame &r_game, bool p_scene_m2b, InputMapper &r_mapper, uint32_t &r_bots, double &r_quit_after, std::string &r_error);

void m2b_gx_load(const M2aGame &p_game, bf3::host::SharedLoadOptions &r_options);

void m2b_gx_server(M2aGame &r_game, bf3::host::GameServerConfig &r_config, uint32_t p_bots);

void m2b_gx_presented(M2aGame &r_game, bf3::host::ClientSession &r_client, const bf3::host::PresentationFrame &p_frame, InputMapper &r_mapper,
		uint64_t p_now_ns);

bool m2b_gx_frame(Bf3LevelSession &r_session, M2aGame &r_game, const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns, double p_delta,
		godot::Camera3D *p_camera, bool p_free_camera, bool p_headless);

void m2b_gx_shutdown(M2aGame &r_game);

}

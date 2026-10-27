#pragma once

#include "death_camera.h"
#include "deploy_screens.h"
#include "hud_tdm.h"
#include "m2b_check.h"
#include "tdm_flow.h"

#include "bf3/host/hud_feed.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;
class NativeFlow;
class NativeScreensCheck;
class LoadingScreenCheck;

struct TdmState {

	bool enabled = false;
	bool playable = false;
	TdmOptions options;
	std::vector<M2bCase> cases;
	M2bCheckOptions check_options;
	bool checks() const { return !cases.empty(); }

	std::unique_ptr<Bf3TdmHud, void (*)(Bf3TdmHud *)> tdm_hud{ nullptr, nullptr };
	UiScreenRecordSet screen_records;
	bf3::host::HudTdmFeedState tdm_state;
	bf3::host::HudKillCardSource kill_card_source;

	std::unique_ptr<Bf3DeathCamera, void (*)(Bf3DeathCamera *)> death_camera{ nullptr, nullptr };

	uint8_t spawn_kit = 0;
	std::array<bf3::host::LoadoutChoice, bf3::host::kKitCount> loadout_choices{};
	bool scoreboard_held = false;
	bool spawned_this_round = false;
	uint64_t spawn_requests_sent = 0;
	bf3::sim::Tick last_request_tick = 0;
	uint32_t own_life_seen = 0;

	TdmFrame frame;
	double ms_tdm_hud = 0, ms_death_camera = 0;

	std::unique_ptr<M2bCheck, void (*)(M2bCheck *)> check{ nullptr, nullptr };
	bool finished = false;

	bool built = false;
	bool windowed = false;
	TdmDeploy deploy;
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
	float view_vfov_deg = 0.0f;
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
	uint32_t loadout_requests_sent = 0;

	bool native_screens = false;
	bool native_screens_check = false;
	bool loading_screen_check = false;
	bool native_following = false;
	bool native_client_set = false;
	bool native_failure_printed = false;
	double native_clock_s = -1.0;
	double native_delta_s = 0.0;
	uint32_t own_spawns = 0;
	uint64_t ui_screens_id = 0;
	std::unique_ptr<NativeFlow, void (*)(NativeFlow *)> native_flow{ nullptr, nullptr };
	DeployScreens deploy_screens;
	std::unique_ptr<NativeScreensCheck, void (*)(NativeScreensCheck *)> native_check{ nullptr, nullptr };
	std::unique_ptr<LoadingScreenCheck, void (*)(LoadingScreenCheck *)> loading_check{ nullptr, nullptr };
};

Bf3UiScreens *tdm_ui_screens(const TdmState &p_m);
}

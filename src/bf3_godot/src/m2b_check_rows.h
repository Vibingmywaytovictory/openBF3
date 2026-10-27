#pragma once

#include "m2b_check.h"

#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

bool m2b_death_had_screen_time(double p_death_t, double p_now_t, double p_round_over_t, double p_need_s);

struct M2bSeekSpot {
	sim::Vec3 position{};
	float yaw = 0.0f;
};
M2bSeekSpot m2b_seek_spot(const sim::Vec3 &p_enemy, float p_enemy_yaw, float p_metres);

struct M2bPlayTally {
	double seconds = 0;
	bool windowed = false;

	bool match_seen = false;
	bool in_round_seen = false;
	bool join_screen_seen = false;
	uint32_t own_deaths = 0;
	uint32_t deaths_checked = 0;
	uint32_t screens_after_death = 0;
	uint32_t deploys = 0;
	uint32_t deploy_kit_mismatch = 0;
	uint32_t kits_deployed_mask = 0;
	uint64_t spawn_requests = 0, spawn_request_resends = 0;
	bool round_over_seen = false;
	uint64_t deaths_received = 0;

	uint64_t feed_lines = 0;
	uint32_t scoreboard_samples = 0, scoreboard_mismatch = 0;
	uint32_t scoreboard_release_samples = 0, scoreboard_release_shown = 0;
	uint32_t scoreboard_round_over_samples = 0, scoreboard_round_over_shown = 0;
	uint32_t kill_cards_checked = 0, kill_cards_bad = 0;
	uint32_t spawn_screen_model_shown = 0;
	bool round_end_shown = false;
	uint64_t missing_lookups = 0;
	uint64_t tag_frames = 0;
	uint64_t tag_icons_painted = 0;
	uint64_t minimap_friendly = 0;

	uint64_t death_views = 0;
	uint64_t death_view_bad_inputs = 0;
};

enum class M2bRowState : uint8_t { Pass = 0, Fail = 1, Skip = 2 };
struct M2bPlayRow {
	std::string name;
	double value = 0.0;
	std::string rule;
	M2bRowState state = M2bRowState::Skip;
	M2bTrack track = M2bTrack::Gx;
	std::string skip_reason;
};

std::vector<M2bPlayRow> m2b_play_rows(const M2bPlayTally &p_tally, bool p_have_ui, bool p_have_cam);

}

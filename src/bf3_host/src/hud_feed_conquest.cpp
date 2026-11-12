#include "hud_feed_tdm_parts.h"

#include "bf3/core/ui/text_db.h"
#include "bf3/sim/logic/conquest_rules.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace bf3::host {

namespace ui = bf3::core::ui;
namespace lg = bf3::sim::logic;

namespace {

constexpr float kSpawnPointMatchMetres = 0.5f;

bool spawnable(const lg::ConquestParams &p_params, size_t p_point) {
	for (const uint32_t node : p_params.points[p_point].spawners) {
		const lg::ConquestSpawnerData *s = p_params.spawner(node);
		if (s != nullptr && !s->vehicle) {
			return true;
		}
	}
	return false;
}

int8_t hud_team(uint8_t p_team, uint8_t p_own_team) {
	if (p_team == 0) {
		return 0;
	}
	return p_team == p_own_team ? int8_t(1) : int8_t(2);
}

}

std::vector<size_t> conquest_flag_order(const lg::ConquestParams &p_params) {
	std::vector<size_t> order;
	for (size_t i = 0; i < p_params.points.size() && i < net::kMaxConquestPoints; ++i) {
		if (p_params.points[i].params.type != lg::CapturableType::UnableToChangeTeam) {
			order.push_back(i);
		}
	}
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return p_params.points[a].params.spawn_menu_list_ordinal < p_params.points[b].params.spawn_menu_list_ordinal;
	});
	return order;
}

uint8_t conquest_spawn_choice(const ui::HudSpawnPoint &p_point, const lg::ConquestParams &p_params) {
	if (p_point.kind != ui::HudSpawnKind::Flag) {
		return 0;
	}
	for (const size_t i : conquest_flag_order(p_params)) {
		const sim::Vec3 &at = p_params.points[i].params.position;
		if (std::hypot(at[0] - p_point.x, at[2] - p_point.z) <= kSpawnPointMatchMetres) {
			return uint8_t(net::kSpawnChoiceFlag + i);
		}
	}
	return 0;
}

namespace tdm_feed {

void feed_conquest(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	ui::HudModel &model = r_frame.model;
	ui::HudCapturePoints &cp = model.capture_points;
	ui::HudTickets &tk = model.tickets;
	cp = ui::HudCapturePoints();
	tk = ui::HudTickets();
	const lg::ConquestParams *params = in.conquest_params;
	if (params == nullptr || !in.conquest_valid) {
		return;
	}
	const net::ConquestState &c = in.conquest;
	if (c.point_count != std::min(params->points.size(), net::kMaxConquestPoints)) {
		++r_frame.state.bad_inputs;
		return;
	}
	const uint8_t my = r_frame.my_team;
	const uint8_t enemy = r_frame.enemy_team;
	tk.team1_tickets = c.tickets[0];
	tk.team2_tickets = c.tickets[1];
	tk.team1_max_tickets = c.initial_tickets[0];
	tk.team2_max_tickets = c.initial_tickets[1];
	tk.friendly_team_tickets = c.tickets[my - 1u];
	tk.enemy_team_tickets = c.tickets[enemy - 1u];
	tk.friendly_team_max_tickets = c.initial_tickets[my - 1u];
	tk.enemy_team_max_tickets = c.initial_tickets[enemy - 1u];
	tk.global_max_tickets = std::max<int32_t>(c.initial_tickets[0], c.initial_tickets[1]);

	std::vector<size_t> order;
	for (size_t i = 0; i < c.point_count; ++i) {
		if (params->points[i].params.type == lg::CapturableType::UnableToChangeTeam) {
			order.push_back(i);
		}
	}
	const std::vector<size_t> flags = conquest_flag_order(*params);
	order.insert(order.end(), flags.begin(), flags.end());
	char letter = 'A';
	ui::HudSpawnPointList &spawns = model.spawn_points.spawn_points;
	for (const size_t i : order) {
		const lg::CapturePointParams &p = params->points[i].params;
		const net::ConquestPointWire &w = c.points[i];
		const bool hq = p.type == lg::CapturableType::UnableToChangeTeam;
		ui::HudCapturePoint h;
		h.x = p.position[0];
		h.z = p.position[2];
		h.owner = w.controlled ? hud_team(w.team, my) : int8_t(0);
		h.attacker = hud_team(w.attacker, my);
		h.hq = hq;
		h.contested = w.under_attack;
		h.progress = float(w.progress) / float(net::kConquestProgressMax);
		h.label_sid_hash = params->points[i].location_sid.empty() ? 0u : ui::text_hash(params->points[i].location_sid);
		h.location_sid_hash = h.label_sid_hash;
		if (cp.capturepoints_status_data.count < ui::kHudMaxCapturePoints) {
			cp.capturepoints_status_data.points[cp.capturepoints_status_data.count++] = h;
		}
		if (hq) {
			continue;
		}
		++cp.total_capturepoints;
		cp.friendly_capturepoints += h.owner == 1 ? 1 : 0;
		cp.enemy_capturepoints += h.owner == 2 ? 1 : 0;
		cp.neutral_capturepoints += h.owner == 0 ? 1 : 0;
		if (lg::capture_point_contains(p, in.own_position) && in.alive) {
			++cp.active_capturepoints;
			cp.capture_in_progress = w.rate != 0;
			cp.captured_by_team = hud_team(w.team, my);
			cp.controlled = w.controlled;
			cp.capture_progress = h.progress;
		}
		if (!spawnable(*params, i)) {
			++letter;
			continue;
		}
		if (spawns.count >= spawns.points.size()) {
			++r_frame.state.rows_dropped;
			++letter;
			continue;
		}
		ui::HudSpawnPoint &s = spawns.points[spawns.count++];
		s.kind = ui::HudSpawnKind::Flag;
		s.team = int8_t(r_frame.own_team);
		s.available = r_frame.match && w.controlled && w.team == r_frame.own_team && r_frame.own_team != 0;
		s.x = p.position[0];
		s.z = p.position[2];
		s.flag_letter = letter++;
		s.label_sid_hash = h.label_sid_hash;
	}
	cp.minimap_capturepoints_status_data = cp.capturepoints_status_data;
}

}

}

#include "bf3/sim/logic/conquest_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::logic {

bool polygon_contains(std::span<const std::array<float, 2>> p_polygon, float p_x, float p_z) {
	bool in = false;
	const size_t n = p_polygon.size();
	for (size_t i = 0, j = n - 1; n >= 3 && i < n; j = i++) {
		const float xi = p_polygon[i][0], zi = p_polygon[i][1];
		const float xj = p_polygon[j][0], zj = p_polygon[j][1];
		if ((zi > p_z) != (zj > p_z) && p_x < (xj - xi) * (p_z - zi) / (zj - zi) + xi) {
			in = !in;
		}
	}
	return in;
}

namespace {

float return_rate(const CapturePointParams &p_params, const CapturePointState &p_state) {
	const float m = p_params.return_multiplier;
	if (p_state.progress == p_state.home || m <= 0.0f) {
		return p_params.lose_control_when_not_close ? -m : 0.0f;
	}
	if (p_state.home == 0.0f) {
		return -m;
	}
	return p_params.lose_control_when_not_close ? -m : m;
}

float player_rate(const CapturePointParams &p_params, uint32_t p_attackers, uint32_t p_defenders) {
	const int64_t a = p_attackers, d = p_defenders;
	const int64_t cap = std::max<int64_t>(p_params.max_capture_multiplier, 0);
	const int64_t min_nr = p_params.min_nr_to_take_control;
	int64_t r = 0;
	if (!p_params.seesaw) {
		if (d == 0 && a >= min_nr) {
			r = -std::min(a, cap);
		} else if (a == 0 && d >= min_nr) {
			r = std::min(d, cap);
		}
	} else {
		const int64_t n = a - d;
		if (n > 0 && a >= min_nr) {
			r = -std::min(n, cap);
		} else if (n < 0 && d >= min_nr) {
			r = std::min(-n, cap);
		}
	}
	if (r == 0) {
		return 0.0f;
	}
	const float magnitude = 1.0f + p_params.added_multiplier_per_player * float(std::llabs(r) - 1);
	return r > 0 ? magnitude : -magnitude;
}

}

float CapturePointParams::area_value_for(uint8_t p_team) const {
	if (!area_values_per_team) {
		return area_value;
	}
	return p_team < area_values.size() ? area_values[p_team] : 0.0f;
}

CapturePointState reset_capture_point(const CapturePointParams &p_params) {
	CapturePointState s;
	s.team = p_params.initial_owner;
	s.controller = s.team;
	s.attacker = s.team;
	s.rate = 0.0f;
	s.home = p_params.start_at_bottom ? 0.0f : kFlagTop;
	s.progress = s.home;
	s.controlled = !p_params.start_at_bottom && s.team != 0;
	s.enabled = p_params.enabled;
	return s;
}

bool capture_point_contains(const CapturePointParams &p_params, const Vec3 &p_point) {
	if (!p_params.volumes.empty()) {
		for (const CaptureVolume &v : p_params.volumes) {
			if (polygon_contains(v.polygon, p_point[0], p_point[2])) {
				return true;
			}
		}
		return false;
	}
	const float dx = p_point[0] - p_params.position[0], dy = p_point[1] - p_params.position[1],
				dz = p_point[2] - p_params.position[2];
	if (p_params.upper_sphere && dy < 0.0f) {
		return false;
	}
	return dx * dx + dy * dy + dz * dz < p_params.capture_radius * p_params.capture_radius;
}

CaptureEvents step_capture_point(const CapturePointParams &p_params, CapturePointState &r_state,
		std::span<const uint32_t, kConquestTeams> p_inside, float p_dt) {
	CaptureEvents ev;
	if (!r_state.enabled || p_params.type == CapturableType::UnableToChangeTeam ||
			(p_params.only_takeable_by_team != 0 && p_params.only_takeable_by_team == r_state.team)) {
		return ev;
	}
	const uint8_t own = r_state.team;
	const uint32_t defenders = own != 0 && own < kConquestTeams ? p_inside[own] : 0;
	uint32_t attackers = 0;
	uint8_t switch_team = 0;
	for (uint8_t t = 1; t < kConquestTeams; ++t) {
		if (t == own || p_inside[t] == 0) {
			continue;
		}
		if (p_inside[t] >= attackers) {
			attackers = p_inside[t];
			switch_team = t;
		}
	}
	r_state.attacker = attackers > 0 ? switch_team : own;

	const float r = attackers == 0 && defenders == 0 ? return_rate(p_params, r_state)
													 : player_rate(p_params, attackers, defenders);
	if (r > 0.0f) {
		if (r_state.progress < kFlagTop) {
			r_state.rate = kFlagTop / float(std::max(p_params.time_to_get_control, 1)) * r;
			r_state.progress += r_state.rate * p_dt;
			if (r_state.progress >= kFlagTop) {
				r_state.progress = kFlagTop;
				r_state.home = kFlagTop;
				r_state.rate = 0.0f;
				if (!r_state.controlled) {
					r_state.controlled = true;
					r_state.controller = r_state.team;
					ev.enable_spawners = true;
					ev.captured = true;
				}
			}
		} else {
			r_state.rate = 0.0f;
		}
	} else if (r < 0.0f) {
		r_state.rate = kFlagTop / float(std::max(p_params.time_to_lose_control, 1)) * r;
		r_state.progress += r_state.rate * p_dt;
		if (r_state.progress <= 0.0f) {
			if (r_state.controlled) {
				r_state.controlled = false;
				ev.lost = true;
			}
			ev.disable_spawners = true;
			r_state.rate = 0.0f;
			r_state.progress = 0.0f;
			r_state.home = 0.0f;
			ev.team_changed = r_state.team != switch_team;
			r_state.team = switch_team;
		}
	} else {
		r_state.rate = 0.0f;
	}
	if (p_params.disable_if_enemy_inside && r_state.team != 0) {
		(attackers == 0 ? ev.enable_spawners : ev.disable_spawners) = true;
	}
	ev.intruder = p_params.intruder_warning && attackers > 0 && r_state.previous_attackers == 0;
	r_state.previous_attackers = attackers;
	r_state.under_attack = r_state.team != 0 && defenders < attackers;
	return ev;
}

void enable_spawner(SpawnerGate &r_gate, uint8_t p_flag_team) {
	if (r_gate.locked_team) {
		r_gate.enabled = r_gate.data_team == p_flag_team;
	} else {
		r_gate.team = p_flag_team;
	}
}

void disable_spawner(SpawnerGate &r_gate) {
	if (r_gate.locked_team) {
		r_gate.enabled = false;
	} else {
		r_gate.team = 0;
	}
}

int64_t TicketCounterState::displayed() const {
	return int64_t(std::trunc(std::max(tickets, 0.0)));
}

double initial_tickets(int64_t p_initial_ticket_count, double p_game_mode_counter_modifier) {
	const double product = double(p_initial_ticket_count) *
			(std::isfinite(p_game_mode_counter_modifier) ? p_game_mode_counter_modifier : 1.0);
	const double whole = std::isfinite(product) ? std::trunc(product) : 1.0;
	return std::clamp(whole, 1.0, 65535.0);
}

TicketCounterState reset_ticket_counter(const TicketCounterParams &p_params, double p_game_mode_counter_modifier) {
	TicketCounterState s;
	s.initial = initial_tickets(p_params.initial_ticket_count, p_game_mode_counter_modifier);
	s.tickets = s.initial;
	return s;
}

void ticket_respawn(const TicketCounterParams &p_params, TicketCounterState &r_state, uint8_t p_slot, uint8_t p_team) {
	r_state.respawn_seen = true;
	if (p_team != p_params.team || p_params.decrease != TicketDecrease::OnRespawned) {
		return;
	}
	const bool first = !r_state.spawned.test(p_slot);
	r_state.spawned.set(p_slot);
	if (!first && r_state.losing()) {
		r_state.tickets = std::max(r_state.tickets - 1.0, 0.0);
		r_state.stopped = r_state.tickets == 0.0;
	}
}

void ticket_killed(
		const TicketCounterParams &p_params, TicketCounterState &r_state, uint8_t p_team, bool p_suppressed) {
	if (p_team != p_params.team || p_params.decrease != TicketDecrease::OnKilled || p_suppressed || !r_state.losing()) {
		return;
	}
	r_state.tickets = std::max(r_state.tickets - 1.0, 0.0);
	r_state.stopped = r_state.tickets == 0.0;
}

void ticket_left(const TicketCounterParams &p_params, TicketCounterState &r_state, uint8_t p_slot, uint8_t p_team) {
	if (p_team == p_params.team) {
		r_state.spawned.reset(p_slot);
	}
}

void ticket_captured(const TicketCounterParams &p_params, TicketCounterState &r_state,
		const CapturePointParams &p_point, const CapturePointState &p_state) {
	if (p_state.team == p_params.team) {
		r_state.area += p_point.area_value_for(p_params.team);
	}
}

void ticket_lost(const TicketCounterParams &p_params, TicketCounterState &r_state, const CapturePointParams &p_point,
		const CapturePointState &p_state) {
	if (p_state.controller == p_params.team) {
		r_state.area -= p_point.area_value_for(p_params.team);
	}
}

void ticket_count_area(
		const TicketCounterParams &p_params, TicketCounterState &r_state, std::span<const ConquestPoint> p_points) {
	r_state.area = 0.0;
	for (const ConquestPoint &p : p_points) {
		if (p.state->team == p_params.team) {
			r_state.area += p.params->area_value_for(p_params.team);
		}
	}
	r_state.area_counted = true;
}

bool team_holds_point(uint8_t p_team, std::span<const ConquestPoint> p_points) {
	for (const ConquestPoint &p : p_points) {
		if (p.state->team == p_team) {
			return true;
		}
	}
	return false;
}

bool enemy_holds_all(uint8_t p_team, std::span<const ConquestPoint> p_points) {
	size_t flags = 0, held = 0;
	for (const ConquestPoint &p : p_points) {
		if (p.params->type == CapturableType::UnableToChangeTeam) {
			continue;
		}
		++flags;
		held += p.state->team != p_team && p.state->controlled && p.state->progress >= kFlagTop ? 1 : 0;
	}
	return flags > 0 && flags == held;
}

bool spawn_allowed(const TicketCounterParams &p_params, uint8_t p_team, std::span<const ConquestPoint> p_points) {
	return p_team != p_params.team || p_params.spawn_always_allowed || team_holds_point(p_team, p_points);
}

TicketEvents ticket_bleed(
		const TicketCounterParams &p_params, TicketCounterState &r_state, const BleedInputs &p_inputs, double p_dt) {
	TicketEvents ev;
	if (p_inputs.bleed_disabled || !r_state.losing()) {
		return ev;
	}
	const double scale = std::isfinite(p_inputs.bleed_scale) && p_inputs.bleed_scale > 0.0 ? p_inputs.bleed_scale : 1.0;
	const auto take = [&](double p_per_second) {
		r_state.tickets = std::max(r_state.tickets - p_per_second * p_dt, 0.0);
		r_state.stopped = r_state.tickets == 0.0;
	};
	const auto bleed = [&](bool p_on) {
		if (p_on && !r_state.bleeding) {
			ev.start_bleed = true;
		} else if (!p_on && r_state.bleeding) {
			ev.stop_bleed = true;
		}
		r_state.bleeding = p_on;
	};
	if (!p_inputs.holds_point) {
		if (!p_inputs.soldier_alive) {
			take(double(p_params.loss_when_lost_all) / 60.0);
			return ev;
		}
		if (p_params.spawn_always_allowed) {
			take(double(p_params.loss_when_lost_all_controls_point) / 60.0);
			return ev;
		}
	}
	const double mine = r_state.area;
	const double enemy = p_inputs.total_area - mine;
	if (p_params.halt_on_equal_point_count && enemy == mine) {
		bleed(false);
		return ev;
	}
	const double diff = enemy - double(p_params.loss_start);
	if (diff < 1.0) {
		bleed(false);
		return ev;
	}
	double base = p_params.loss_per_min;
	if (p_inputs.enemy_holds_all) {
		if (!r_state.all_held) {
			r_state.all_held = true;
			ev.all_held = true;
		}
	} else if (!p_params.percentages.empty()) {
		const double percent = r_state.initial > 0.0 ? r_state.tickets / r_state.initial * 100.0 : 0.0;
		for (size_t i = 0; i < p_params.percentages.size(); ++i) {
			if (percent <= double(p_params.percentages[i].max_percent_of_tickets_left)) {
				base = p_params.percentages[i].ticket_loss_per_min;
				ev.percentage_step = r_state.percentage_step != i + 1;
				r_state.percentage_step = i + 1;
				break;
			}
		}
	}
	take(base * (1.0 + 0.01 * diff) / 60.0 * scale);
	bleed(true);
	return ev;
}

bool combat_area_tracks(const CombatAreaParams &p_params, const CombatAreaPlayer &p_player, bool p_deserting_allowed) {
	if (p_deserting_allowed || !p_player.alive) {
		return false;
	}
	if (p_params.team_specific && p_player.team != p_params.team) {
		return false;
	}
	return !(p_player.ai && p_params.ai_allowed_outside);
}

CombatAreaEvents step_combat_area(const CombatAreaParams &p_params, std::vector<Deserter> &r_deserters,
		std::span<const CombatAreaPlayer> p_players, bool p_deserting_allowed, float p_dt) {
	CombatAreaEvents ev;
	std::bitset<kConquestSlots> tracked;
	for (const CombatAreaPlayer &p : p_players) {
		if (!combat_area_tracks(p_params, p, p_deserting_allowed)) {
			continue;
		}
		tracked.set(p.slot);
		const auto listed = std::find_if(
				r_deserters.begin(), r_deserters.end(), [&](const Deserter &d) { return d.slot == p.slot; });
		if (p.inside && listed != r_deserters.end()) {
			ev.returned.push_back(p.slot);
			r_deserters.erase(listed);
		} else if (!p.inside && listed == r_deserters.end()) {
			Deserter d;
			d.slot = p.slot;
			d.timer = float(p_params.time_to_return);
			r_deserters.push_back(d);
			ev.entered.push_back(d);
		}
	}
	std::vector<Deserter> kept;
	for (Deserter &d : r_deserters) {
		if (!tracked.test(d.slot)) {
			continue;
		}
		d.timer -= p_dt;
		if (d.timer <= 1.0f && !d.struck) {
			d.struck = true;
			ev.strikes.push_back(d.slot);
		}
		if (d.timer <= 0.0f) {
			ev.kills.push_back(d.slot);
			continue;
		}
		kept.push_back(d);
	}
	r_deserters.swap(kept);
	return ev;
}

}

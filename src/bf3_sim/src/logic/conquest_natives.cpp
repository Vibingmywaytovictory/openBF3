#include "bf3/sim/logic/conquest_natives.h"

#include "bf3/core/gameplay/logic_types.h"

#include <algorithm>
#include <array>
#include <string>

namespace bf3::sim::logic {

namespace gp = core::gameplay;
using T = gp::LogicTypeId;

namespace {

constexpr int32_t id(std::string_view p_name) {
	return gp::logic_port_id(p_name);
}

constexpr int32_t kTickets = id("Tickets");
constexpr int32_t kEnable = id("Enable");
constexpr int32_t kDisable = id("Disable");
constexpr int32_t kEnabled = id("Enabled");
constexpr int32_t kStartRound = id("StartRound");
constexpr int32_t kReset = id("Reset");
constexpr int32_t kEnableCapture = id("EnableCapture");
constexpr int32_t kDisableCapture = id("DisableCapture");
constexpr int32_t kDisableSpawners = id("DisableSpawners");
constexpr int32_t kOnStartBleed = id("OnStartBleed");
constexpr int32_t kOnStopBleed = id("OnStopBleed");
constexpr int32_t kInitialTicketCount = id("InitialTicketCount");
constexpr int32_t kAreaValue = id("AreaValue");
constexpr int32_t kInitialOwnerTeam = id("InitialOwnerTeam");

double value_number(const LogicValue &p_value) {
	switch (p_value.kind) {
		case LogicValue::Kind::Bool:
			return p_value.b ? 1.0 : 0.0;
		case LogicValue::Kind::Float:
			return p_value.f;
		default:
			return double(p_value.i);
	}
}

bool value_on(const LogicValue &p_value) {
	switch (p_value.kind) {
		case LogicValue::Kind::Bool:
			return p_value.b;
		case LogicValue::Kind::Float:
			return p_value.f != 0.0;
		default:
			return p_value.i != 0;
	}
}

}

struct ConquestNatives::State {
	std::shared_ptr<const gp::LogicGraph> graph;
	std::shared_ptr<const ConquestParams> params;
	ConquestOptions options;

	struct Point {
		const ConquestPointData *data = nullptr;
		CapturePointState state;
		bool entity_enabled = true;
	};
	std::vector<Point> points;
	struct Counter {
		const ConquestCounterData *data = nullptr;
		TicketCounterState state;
		int64_t published = -1;
		bool enabled = true;
	};
	std::vector<Counter> counters;
	struct Area {
		const ConquestAreaData *data = nullptr;
		std::vector<Deserter> deserters;
		bool enabled = true;
	};
	std::vector<Area> areas;
	struct Gate {
		uint32_t node = 0;
		SpawnerGate gate;
	};
	std::vector<Gate> gates;
	std::vector<ConquestPointView> views;
	std::vector<uint8_t> kills;
	ConquestStats stats;
	bool started = false;

	void unhandled(uint32_t p_node, int32_t p_port) {
		++stats.unhandled_inputs;
		if (stats.unhandled.size() < 32) {
			stats.unhandled.push_back({ p_node, p_port });
		}
	}

	Gate *gate(uint32_t p_node) {
		for (Gate &g : gates) {
			if (g.node == p_node) {
				return &g;
			}
		}
		return nullptr;
	}

	std::vector<ConquestPoint> rule_points() const {
		std::vector<ConquestPoint> out;
		out.reserve(points.size());
		for (const Point &p : points) {
			out.push_back({ &p.data->params, &p.state });
		}
		return out;
	}

	void switch_spawners(const Point &p_point, bool p_enable) {
		for (const uint32_t node : p_point.data->spawners) {
			if (Gate *g = gate(node)) {
				if (p_enable) {
					enable_spawner(g->gate, p_point.state.team);
				} else {
					disable_spawner(g->gate);
				}
			}
		}
	}

	void set_enablable(uint8_t p_team, bool p_on) {
		for (Point &p : points) {
			if (p.data->params.type == CapturableType::CaptureEnablable && p.state.team == p_team) {
				p.state.enabled = p_on;
			}
		}
	}

	core::Status publish(Counter &r_counter, LogicRuntime &r_runtime) {
		const int64_t shown = r_counter.state.displayed();
		if (shown != r_counter.published) {
			r_counter.published = shown;
			LogicValue v;
			v.kind = LogicValue::Kind::Int;
			v.i = shown;
			BF3_TRY(r_runtime.force_property(r_counter.data->node, kTickets, v));
		}
		return core::Status::success();
	}

	core::Status start(LogicRuntime &r_runtime) {
		started = true;
		for (const Point &p : points) {
			if (p.entity_enabled) {
				switch_spawners(p, true);
			}
		}
		const std::vector<ConquestPoint> rp = rule_points();
		for (Counter &c : counters) {
			ticket_count_area(c.data->params, c.state, rp);
			set_enablable(c.data->params.team, c.state.area == 0.0);
			BF3_TRY(publish(c, r_runtime));
		}
		return core::Status::success();
	}

	void refresh_views() {
		views.resize(points.size());
		for (size_t i = 0; i < points.size(); ++i) {
			const Point &p = points[i];
			ConquestPointView &v = views[i];
			v.node = p.data->node;
			v.level_object = p.data->level_object;
			v.team = p.state.team;
			v.attacker = p.state.attacker;
			v.progress = p.state.progress;
			v.rate = p.state.rate;
			v.controlled = p.state.controlled;
			v.under_attack = p.state.under_attack;
			v.hq = p.data->params.type == CapturableType::UnableToChangeTeam;
			v.ordinal = p.data->params.spawn_menu_list_ordinal;
			v.position = p.data->params.position;
		}
	}
};

ConquestNatives::ConquestNatives(std::unique_ptr<State> p_state) : state_(std::move(p_state)) {}
ConquestNatives::ConquestNatives(ConquestNatives &&) noexcept = default;
ConquestNatives &ConquestNatives::operator=(ConquestNatives &&) noexcept = default;
ConquestNatives::~ConquestNatives() = default;

core::Result<ConquestNatives> ConquestNatives::create(std::shared_ptr<const gp::LogicGraph> p_graph,
		std::shared_ptr<const ConquestParams> p_params, const ConquestOptions &p_options) {
	if (!p_graph || !p_params) {
		return core::make_error("conquest natives: no graph or no conquest params");
	}
	auto s = std::make_unique<State>();
	s->graph = std::move(p_graph);
	s->params = std::move(p_params);
	s->options = p_options;
	const gp::LogicGraph &g = *s->graph;
	for (const ConquestPointData &d : s->params->points) {
		if (d.node >= g.nodes().size() || g.nodes()[d.node].type != T::CapturePoint) {
			return core::make_error(
					"conquest natives: point node " + std::to_string(d.node) + " is not a CapturePoint");
		}
		State::Point p;
		p.data = &d;
		p.state = reset_capture_point(d.params);
		p.entity_enabled = d.params.enabled;
		s->points.push_back(p);
	}
	for (const ConquestCounterData &d : s->params->counters) {
		if (d.node >= g.nodes().size() || g.nodes()[d.node].type != T::TicketCounter) {
			return core::make_error(
					"conquest natives: counter node " + std::to_string(d.node) + " is not a TicketCounter");
		}
		State::Counter c;
		c.data = &d;
		c.state = reset_ticket_counter(d.params, p_options.game_mode_counter_modifier);
		s->counters.push_back(std::move(c));
	}
	for (const ConquestAreaData &d : s->params->areas) {
		State::Area a;
		a.data = &d;
		s->areas.push_back(std::move(a));
	}
	for (const ConquestSpawnerData &d : s->params->spawners) {
		s->gates.push_back({ d.node, d.gate });
	}
	s->refresh_views();
	return ConquestNatives(std::move(s));
}

bool ConquestNatives::handles(uint32_t p_node) const {
	const State &s = *state_;
	const auto has = [&](const auto &p_list) {
		return std::any_of(p_list.begin(), p_list.end(), [&](const auto &e) { return e.data->node == p_node; });
	};
	return has(s.points) || has(s.counters) || has(s.areas) ||
			std::any_of(s.gates.begin(), s.gates.end(), [&](const State::Gate &g) { return g.node == p_node; });
}

core::Status ConquestNatives::effect(const LogicEffect &p_effect, LogicRuntime &r_runtime) {
	State &s = *state_;
	const bool property = p_effect.kind == LogicEffectKind::Property;
	const int32_t port = p_effect.port;
	const auto switched = [&](bool &r_flag) {
		if (property && port == kEnabled) {
			r_flag = value_on(p_effect.value);
		} else if (!property && port == kEnable) {
			r_flag = true;
		} else if (!property && port == kDisable) {
			r_flag = false;
		} else {
			return false;
		}
		return true;
	};
	for (State::Counter &c : s.counters) {
		if (c.data->node != p_effect.node) {
			continue;
		}
		if (switched(c.enabled)) {
			return core::Status::success();
		}
		if (!property && port == kStartRound) {
			c.state.round_started = true;
		} else if (property && port == kInitialTicketCount &&
				value_number(p_effect.value) == double(c.data->params.initial_ticket_count)) {
		} else if (!property && port == kReset) {
			c.state = reset_ticket_counter(c.data->params, s.options.game_mode_counter_modifier);
			if (c.data->params.reset_capture_points_on_reset) {
				for (State::Point &p : s.points) {
					if (p.state.team == c.data->params.team) {
						p.state = reset_capture_point(p.data->params);
					}
				}
			}
			BF3_TRY(s.publish(c, r_runtime));
		} else {
			s.unhandled(p_effect.node, port);
		}
		return core::Status::success();
	}
	for (State::Point &p : s.points) {
		if (p.data->node != p_effect.node) {
			continue;
		}
		if (switched(p.entity_enabled)) {
			return core::Status::success();
		}
		if (!property && port == kEnableCapture) {
			p.state.enabled = true;
		} else if (!property && port == kDisableCapture) {
			p.state.enabled = false;
		} else if (property && port == kAreaValue &&
				value_number(p_effect.value) == double(p.data->params.area_value)) {
		} else if (property && port == kInitialOwnerTeam &&
				value_number(p_effect.value) == double(p.data->params.initial_owner)) {
		} else if (!property && port == kDisableSpawners) {
			s.switch_spawners(p, false);
		} else {
			s.unhandled(p_effect.node, port);
		}
		return core::Status::success();
	}
	for (State::Area &a : s.areas) {
		if (a.data->node == p_effect.node) {
			if (!switched(a.enabled)) {
				s.unhandled(p_effect.node, port);
			}
			return core::Status::success();
		}
	}
	if (State::Gate *g = s.gate(p_effect.node)) {
		switched(g->gate.enabled);
	}
	return core::Status::success();
}

core::Status ConquestNatives::step(
		Tick p_tick, float p_dt, std::span<const ConquestPlayerSample> p_players, LogicRuntime &r_runtime) {
	(void)p_tick;
	State &s = *state_;
	if (!s.started) {
		BF3_TRY(s.start(r_runtime));
	}
	for (State::Point &p : s.points) {
		if (!p.entity_enabled) {
			continue;
		}
		std::array<uint32_t, kConquestTeams> inside{};
		for (const ConquestPlayerSample &pl : p_players) {
			if (pl.alive && pl.team != 0 && pl.team < kConquestTeams &&
					capture_point_contains(p.data->params, pl.position)) {
				++inside[pl.team];
			}
		}
		const CaptureEvents ev =
				step_capture_point(p.data->params, p.state, std::span<const uint32_t, kConquestTeams>(inside), p_dt);
		if (ev.captured) {
			++s.stats.captures;
			for (State::Counter &c : s.counters) {
				const bool from_zero = c.state.area == 0.0;
				ticket_captured(c.data->params, c.state, p.data->params, p.state);
				if (from_zero && c.state.area != 0.0) {
					s.set_enablable(c.data->params.team, false);
				}
			}
			BF3_TRY(r_runtime.force_event(p.data->node, gp::logic_port::kOnCaptured, LogicContext{ -1, p.state.team }));
		}
		if (ev.lost) {
			++s.stats.losses;
			for (State::Counter &c : s.counters) {
				const bool had = c.state.area != 0.0;
				ticket_lost(c.data->params, c.state, p.data->params, p.state);
				if (had && c.state.area == 0.0) {
					s.set_enablable(c.data->params.team, true);
				}
			}
			BF3_TRY(r_runtime.force_event(
					p.data->node, gp::logic_port::kOnLost, LogicContext{ -1, p.state.controller }));
		}
		if (ev.disable_spawners) {
			s.switch_spawners(p, false);
		}
		if (ev.enable_spawners) {
			s.switch_spawners(p, true);
		}
	}
	const std::vector<ConquestPoint> rp = s.rule_points();
	double total = 0.0;
	for (const State::Counter &c : s.counters) {
		total += c.state.area;
	}
	for (State::Counter &c : s.counters) {
		if (!c.enabled) {
			continue;
		}
		const uint8_t team = c.data->params.team;
		BleedInputs in;
		in.total_area = total;
		in.holds_point = team_holds_point(team, rp);
		in.soldier_alive = std::any_of(p_players.begin(), p_players.end(),
				[&](const ConquestPlayerSample &pl) { return pl.alive && pl.team == team; });
		in.enemy_holds_all = enemy_holds_all(team, rp);
		in.bleed_disabled = s.options.bleed_disabled;
		const TicketEvents ev = ticket_bleed(c.data->params, c.state, in, double(p_dt));
		if (ev.start_bleed) {
			BF3_TRY(r_runtime.force_event(c.data->node, kOnStartBleed, LogicContext{ -1, team }));
		}
		if (ev.stop_bleed) {
			BF3_TRY(r_runtime.force_event(c.data->node, kOnStopBleed, LogicContext{ -1, team }));
		}
		BF3_TRY(s.publish(c, r_runtime));
	}
	for (State::Area &a : s.areas) {
		if (!a.enabled || a.data->polygon.size() < 3) {
			continue;
		}
		std::vector<CombatAreaPlayer> players;
		players.reserve(p_players.size());
		for (const ConquestPlayerSample &pl : p_players) {
			CombatAreaPlayer cp;
			cp.slot = pl.slot;
			cp.team = pl.team;
			cp.alive = pl.alive;
			cp.ai = pl.ai;
			cp.inside = polygon_contains(a.data->polygon, pl.position[0], pl.position[2]);
			players.push_back(cp);
		}
		const CombatAreaEvents ev =
				step_combat_area(a.data->params, a.deserters, players, s.options.deserting_allowed, p_dt);
		s.stats.strikes += ev.strikes.size();
		s.stats.deserter_kills += ev.kills.size();
		s.kills.insert(s.kills.end(), ev.kills.begin(), ev.kills.end());
	}
	s.refresh_views();
	return core::Status::success();
}

void ConquestNatives::respawned(uint8_t p_slot, uint8_t p_team) {
	for (State::Counter &c : state_->counters) {
		const double before = c.state.tickets;
		ticket_respawn(c.data->params, c.state, p_slot, p_team);
		state_->stats.ticket_respawn_costs += c.state.tickets < before ? 1 : 0;
	}
}

void ConquestNatives::killed(uint8_t p_team) {
	for (State::Counter &c : state_->counters) {
		ticket_killed(c.data->params, c.state, p_team, false);
	}
}

void ConquestNatives::left(uint8_t p_slot, uint8_t p_team) {
	for (State::Counter &c : state_->counters) {
		ticket_left(c.data->params, c.state, p_slot, p_team);
	}
	for (State::Area &a : state_->areas) {
		std::erase_if(a.deserters, [&](const Deserter &d) { return d.slot == p_slot; });
	}
}

int64_t ConquestNatives::tickets(uint8_t p_team) const {
	for (const State::Counter &c : state_->counters) {
		if (c.data->params.team == p_team) {
			return c.state.displayed();
		}
	}
	return 0;
}

int64_t ConquestNatives::initial_tickets(uint8_t p_team) const {
	for (const State::Counter &c : state_->counters) {
		if (c.data->params.team == p_team) {
			return int64_t(c.state.initial);
		}
	}
	return 0;
}

bool ConquestNatives::bleeding(uint8_t p_team) const {
	for (const State::Counter &c : state_->counters) {
		if (c.data->params.team == p_team) {
			return c.state.bleeding;
		}
	}
	return false;
}

bool ConquestNatives::spawn_allowed(uint8_t p_team) const {
	const std::vector<ConquestPoint> rp = state_->rule_points();
	for (const State::Counter &c : state_->counters) {
		if (!logic::spawn_allowed(c.data->params, p_team, rp)) {
			return false;
		}
	}
	return true;
}

bool ConquestNatives::spawner_open(uint32_t p_node, uint8_t p_team) const {
	for (const State::Gate &g : state_->gates) {
		if (g.node == p_node) {
			return g.gate.locked_team ? g.gate.enabled && g.gate.data_team == p_team
									  : g.gate.enabled && g.gate.team == p_team;
		}
	}
	return false;
}

bool ConquestNatives::spawn_point_open(uint32_t p_spawner_object, uint8_t p_team) const {
	for (const ConquestSpawnerData &d : state_->params->spawners) {
		if (!d.vehicle && d.level_object == p_spawner_object) {
			return spawner_open(d.node, p_team);
		}
	}
	return true;
}

std::span<const ConquestPointView> ConquestNatives::points() const {
	return state_->views;
}

std::span<const Deserter> ConquestNatives::deserters(uint8_t p_team) const {
	for (const State::Area &a : state_->areas) {
		if (a.data->params.team == p_team || !a.data->params.team_specific) {
			return a.deserters;
		}
	}
	return {};
}

std::vector<uint8_t> ConquestNatives::take_kills() {
	std::vector<uint8_t> out;
	out.swap(state_->kills);
	return out;
}

const ConquestStats &ConquestNatives::stats() const {
	return state_->stats;
}

}

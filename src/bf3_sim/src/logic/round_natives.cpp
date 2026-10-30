#include "bf3/sim/logic/round_natives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace bf3::sim::logic {

namespace gp = core::gameplay;
using T = LogicTypeId;
using D = NativePortDir;
using M = NativePortMeaning;

namespace {

constexpr int32_t id(std::string_view p_name) {
	return gp::logic_port_id(p_name);
}

constexpr int32_t kKills = id("Kills");
constexpr int32_t kKillLimit = id("KillLimit");
constexpr int32_t kStartRound = id("StartRound");
constexpr int32_t kEnablePlayerMovement = id("EnablePlayerMovement");
constexpr int32_t kOnPlayerSpawned = id("OnPlayerSpawned");
constexpr int32_t kOnPlayerDied = id("OnPlayerDied");
constexpr int32_t kOnSpectatorEnter = id("OnSpectatorEnter");
constexpr int32_t kTeamField = id("Team");

constexpr size_t kSlots = 256;

bool handled_type(T p_type) {
	switch (p_type) {
		case T::KillCounter:
		case T::PreRound:
		case T::RoundOver:
		case T::HumanPlayer:
		case T::AutoTeam:
		case T::InputRestriction:
		case T::LevelControl:
		case T::CharacterSpawn:
		case T::AlternateSpawn:
			return true;
		default:
			return false;
	}
}

bool is_native(T p_type) {
	return gp::logic_type_info(p_type).cls == gp::LogicTypeClass::Native;
}

bool value_bool(const LogicValue &p_value) {
	switch (p_value.kind) {
		case LogicValue::Kind::Bool:
			return p_value.b;
		case LogicValue::Kind::Float:
			return p_value.f != 0.0;
		default:
			return p_value.i != 0;
	}
}

LogicValue int_value(int64_t p_value) {
	LogicValue v;
	v.kind = LogicValue::Kind::Int;
	v.i = p_value;
	return v;
}

}

struct RoundNatives::State {
	std::shared_ptr<const gp::LogicGraph> graph;
	RoundNativeConfig config;
	RoundNativeParams params;
	std::optional<LogicNativeStubs> stubs;

	struct Counter {
		uint32_t node = 0;
		uint8_t team = 0;
		int32_t max_kill_count = 100;
		double enemy_weight = 1.0;
		bool ignore_ai = false;
		double kills = 0;
		int64_t published = -1;
	};
	std::vector<Counter> counters;
	struct Over {
		uint32_t node = 0;
		uint8_t team = 0;
		bool armed = false;
	};
	std::vector<Over> overs;
	std::vector<uint32_t> pre_rounds, humans;
	std::vector<bool> enabled;

	RoundPhase phase = RoundPhase::PreRound;
	uint8_t winner = 0;
	bool restricted = false;
	bool counting = false;
	Tick countdown_end = 0;
	bool unspawn_flag = false;
	bool restart_flag = false;
	bool level_end_flag = false;
	bool published = false;
	Tick tick = 0;

	std::array<uint8_t, kSlots> team{};
	std::array<bool, kSlots> entered{};
	std::array<bool, kSlots> bot{};
	std::array<uint32_t, kSlots> join{};
	uint32_t join_seq = 0;
	int balance_candidate = -1;
	uint32_t moves = 0;

	std::vector<NativeTimelineEntry> timeline;
	bool timeline_full = false;
	std::vector<NativeCoverage> coverage;

	void note(uint32_t p_node, NativeTimelineEntry::What p_what, int64_t p_value) {
		if (timeline.size() >= kMaxNativeTimelineEntries) {
			timeline_full = true;
			return;
		}
		NativeTimelineEntry e;
		e.tick = tick;
		e.node = p_node;
		e.what = p_what;
		e.value = p_value;
		timeline.push_back(e);
	}

	NativeCoverage *coverage_of(T p_type) {
		for (NativeCoverage &c : coverage) {
			if (c.type == p_type) {
				return &c;
			}
		}
		return nullptr;
	}

	int32_t limit_of(const Counter &p_counter) const { return config.kill_limit_override > 0 ? config.kill_limit_override : p_counter.max_kill_count; }

	core::Status publish_counter(Counter &r_counter, LogicRuntime &r_runtime) {
		const int64_t k = int64_t(std::floor(r_counter.kills + 1e-9));
		if (k != r_counter.published) {
			r_counter.published = k;
			BF3_TRY(r_runtime.force_property(r_counter.node, kKills, int_value(k)));
		}
		return core::Status::success();
	}

	uint32_t team_count(uint8_t p_team) const {
		uint32_t n = 0;
		for (size_t i = 0; i < kSlots; ++i) {
			n += entered[i] && team[i] == p_team ? 1u : 0u;
		}
		return n;
	}

	uint32_t player_count() const { return team_count(1) + team_count(2); }

	uint8_t choose_team() const {
		const uint32_t cap = std::max<uint32_t>(1u, uint32_t(config.max_players) / 2u);
		const uint32_t a = team_count(1), b = team_count(2);
		if (a >= cap && b >= cap) {
			return 0;
		}
		if (a >= cap) {
			return 2;
		}
		if (b >= cap) {
			return 1;
		}
		return b < a ? 2 : 1;
	}

	void pick_balance_candidate() {
		balance_candidate = -1;
		if (!params.auto_balance) {
			return;
		}
		const uint32_t a = team_count(1), b = team_count(2);
		if (int32_t(a + b) < params.balance_player_count) {
			return;
		}
		const int32_t diff = int32_t(a) - int32_t(b);
		if (std::abs(diff) < std::max(1, params.balance_team_difference)) {
			return;
		}
		const uint8_t larger = diff > 0 ? 1 : 2;
		int best_bot = -1, best_human = -1;
		for (size_t i = 0; i < kSlots; ++i) {
			if (!entered[i] || team[i] != larger) {
				continue;
			}
			int &best = bot[i] ? best_bot : best_human;
			if (best < 0 || join[i] > join[size_t(best)]) {
				best = int(i);
			}
		}
		balance_candidate = best_bot >= 0 ? best_bot : best_human;
	}
};

RoundNatives::RoundNatives(std::unique_ptr<State> p_state) :
		state_(std::move(p_state)) {}
RoundNatives::RoundNatives(RoundNatives &&) noexcept = default;
RoundNatives &RoundNatives::operator=(RoundNatives &&) noexcept = default;
RoundNatives::~RoundNatives() = default;

core::Result<RoundNatives> RoundNatives::create(std::shared_ptr<const gp::LogicGraph> p_graph, const RoundNativeConfig &p_config) {
	if (!p_graph) {
		return core::make_error("round natives: no graph");
	}
	if (p_config.max_players == 0) {
		return core::make_error("round natives: max_players 0");
	}
	auto state = std::make_unique<State>();
	state->graph = p_graph;
	state->config = p_config;
	state->params = p_config.params ? *p_config.params : RoundNativeParams{};
	core::Result<LogicNativeStubs> stubs = LogicNativeStubs::create(p_graph);
	if (!stubs) {
		return stubs.error();
	}
	state->stubs.emplace(std::move(*stubs));
	const gp::LogicGraph &g = *p_graph;
	state->enabled.assign(g.nodes().size(), false);
	uint8_t next_counter_team = 1;
	for (uint32_t i = 0; i < g.nodes().size(); ++i) {
		const T type = g.nodes()[i].type;
		if (!is_native(type)) {
			continue;
		}
		if (state->coverage_of(type) == nullptr) {
			NativeCoverage c;
			c.type = type;
			state->coverage.push_back(c);
		}
		const std::optional<LogicValue> e = g.param(i, gp::logic_port::kEnabled);
		state->enabled[i] = e ? value_bool(*e) : true;
		switch (type) {
			case T::KillCounter: {
				State::Counter c;
				c.node = i;
				c.team = next_counter_team;
				next_counter_team = next_counter_team == 1 ? 2 : 1;
				for (const RoundNativeParams::KillCounter &k : state->params.kill_counters) {
					if (k.node == i) {
						c.team = k.team == 1 || k.team == 2 ? k.team : c.team;
						c.max_kill_count = std::clamp(k.max_kill_count, 1, 1000000);
						c.enemy_weight = std::isfinite(k.enemy_weight) ? std::clamp(double(k.enemy_weight), 0.0, 1000.0) : 1.0;
						c.ignore_ai = k.ignore_ai;
					}
				}
				state->counters.push_back(c);
				break;
			}
			case T::RoundOver: {
				State::Over o;
				o.node = i;
				if (const std::optional<LogicValue> t = g.param(i, kTeamField)) {
					o.team = t->i == 1 || t->i == 2 ? uint8_t(t->i) : 0;
				}
				state->overs.push_back(o);
				break;
			}
			case T::PreRound:
				state->pre_rounds.push_back(i);
				break;
			case T::HumanPlayer:
				state->humans.push_back(i);
				break;
			default:
				break;
		}
	}
	std::sort(state->coverage.begin(), state->coverage.end(), [](const NativeCoverage &a, const NativeCoverage &b) { return a.type < b.type; });

	RoundNativeParams &p = state->params;
	p.round_min_players = std::clamp(p.round_min_players, 0, 256);
	p.round_max_players = std::clamp(p.round_max_players, 0, 256);
	if (!std::isfinite(p.round_restart_countdown_s) || p.round_restart_countdown_s < 0.0f) {
		p.round_restart_countdown_s = 10.0f;
	}
	p.round_restart_countdown_s = std::min(p.round_restart_countdown_s, 3600.0f);
	if (!std::isfinite(p.player_killed_delay_s) || p.player_killed_delay_s < 0.0f) {
		p.player_killed_delay_s = 5.0f;
	}
	p.player_killed_delay_s = std::min(p.player_killed_delay_s, 3600.0f);
	return RoundNatives(std::move(state));
}

core::Status RoundNatives::apply(Tick p_tick, std::span<const LogicEffect> p_effects, LogicRuntime &r_runtime) {
	State &s = *state_;
	s.tick = p_tick;
	const gp::LogicGraph &g = *s.graph;
	if (!s.published) {

		s.published = true;
		for (State::Counter &c : s.counters) {
			BF3_TRY(r_runtime.force_property(c.node, kKillLimit, int_value(s.limit_of(c))));
			BF3_TRY(s.publish_counter(c, r_runtime));
		}
	}
	std::vector<LogicEffect> rest;
	int32_t wins_node = -1;
	for (const LogicEffect &e : p_effects) {
		if (e.apply_tick != p_tick) {
			continue;
		}
		if (e.node >= g.nodes().size() || !is_native(g.nodes()[e.node].type)) {
			return core::make_error("round natives: effect into node " + std::to_string(e.node) + ", which is not native");
		}
		const T type = g.nodes()[e.node].type;
		NativeCoverage *cov = s.coverage_of(type);
		if (!handled_type(type)) {
			if (cov != nullptr) {
				++cov->recorded;
			}
			rest.push_back(e);
			continue;
		}
		const D dir = e.kind == LogicEffectKind::Property ? D::PropertyIn : D::EventIn;
		const std::optional<NativePortBinding> b = native_port(type, dir, e.port);
		const M meaning = b ? b->meaning : M::Recorded;
		bool handled = true;
		switch (meaning) {
			case M::Enabled:
			case M::Enable:
			case M::Disable: {
				const bool on = meaning == M::Enable || (meaning == M::Enabled && value_bool(e.value));
				if (s.enabled[e.node] != on) {
					s.enabled[e.node] = on;
					s.note(e.node, NativeTimelineEntry::What::Enabled, on ? 1 : 0);
				}
				if (type == T::InputRestriction && !on) {
					s.restricted = false;
				}
				break;
			}
			case M::RoundStarted:
				for (State::Over &o : s.overs) {
					if (o.node == e.node) {
						o.armed = true;
					}
				}
				s.note(e.node, NativeTimelineEntry::What::RoundOver, 0);
				break;
			case M::TeamWins:
				for (const State::Over &o : s.overs) {
					if (o.node == e.node && o.armed && s.winner == 0 && s.phase == RoundPhase::InRound && o.team != 0 &&
							(wins_node < 0 || int32_t(e.node) < wins_node)) {
						wins_node = int32_t(e.node);
					}
				}
				break;
			case M::StopTime:
				s.note(e.node, NativeTimelineEntry::What::Input, e.port);
				break;
			case M::UnSpawnAllSoldiers:
				s.unspawn_flag = true;
				s.note(e.node, NativeTimelineEntry::What::Input, e.port);
				break;
			case M::PlayerEnter:
			case M::DisableAllowedToSpawnOn:

				break;
			case M::Activate:
				if (type == T::InputRestriction && s.enabled[e.node]) {
					s.restricted = true;
				}
				break;
			case M::Deactivate:
				if (type == T::InputRestriction) {
					s.restricted = false;
				}
				break;
			case M::RestartLevel:
				s.restart_flag = true;
				s.phase = RoundPhase::Restarting;
				s.note(e.node, NativeTimelineEntry::What::Input, e.port);
				break;
			default:

				if (type == T::LevelControl && dir == D::EventIn) {
					s.level_end_flag = true;
					s.phase = RoundPhase::Restarting;
					s.note(e.node, NativeTimelineEntry::What::Input, e.port);
				} else if ((type == T::CharacterSpawn || type == T::AlternateSpawn) && dir == D::PropertyIn &&
						e.port == id("BlueprintTransform")) {

				} else {
					handled = false;
				}
				break;
		}
		if (cov != nullptr) {
			if (handled) {
				++cov->handled;
			} else {
				++cov->recorded;
				s.note(e.node, NativeTimelineEntry::What::Input, e.port);
			}
		}
	}
	if (wins_node >= 0) {
		for (const State::Over &o : s.overs) {
			if (int32_t(o.node) == wins_node) {
				s.winner = o.team;
			}
		}
		s.phase = RoundPhase::RoundOver;
		s.note(uint32_t(wins_node), NativeTimelineEntry::What::RoundOver, s.winner);
		BF3_TRY(r_runtime.force_event(uint32_t(wins_node), gp::logic_port::kOnRoundOver, LogicContext{ -1, s.winner }));
	}
	BF3_TRY(s.stubs->apply(p_tick, rest, r_runtime));
	if (s.timeline_full) {
		return core::make_error("round natives: the timeline holds more than " + std::to_string(kMaxNativeTimelineEntries) + " entries");
	}
	return core::Status::success();
}

core::Status RoundNatives::player_enter(uint8_t p_slot, bool p_bot, LogicRuntime &r_runtime) {
	State &s = *state_;
	if (s.entered[p_slot]) {
		return core::Status::success();
	}
	return player_enter_team(p_slot, p_bot, s.choose_team(), r_runtime);
}

core::Status RoundNatives::player_enter_team(uint8_t p_slot, bool p_bot, uint8_t p_team, LogicRuntime &r_runtime) {
	State &s = *state_;
	s.entered[p_slot] = true;
	s.bot[p_slot] = p_bot;
	s.team[p_slot] = p_team == 1 || p_team == 2 ? p_team : 0;
	s.join[p_slot] = ++s.join_seq;
	const LogicContext ctx{ int32_t(p_slot), s.team[p_slot] };
	for (const uint32_t h : s.humans) {
		BF3_TRY(r_runtime.force_event(h, s.team[p_slot] != 0 ? gp::logic_port::kOnPlayerEnter : kOnSpectatorEnter, ctx));
	}
	return core::Status::success();
}

core::Status RoundNatives::player_switch_team(uint8_t p_slot, uint8_t p_team) {
	State &s = *state_;
	if (!s.entered[p_slot] || (p_team != 1 && p_team != 2)) {
		return core::make_error(
				"round natives: slot " + std::to_string(p_slot) + " cannot switch to team " + std::to_string(p_team));
	}
	s.team[p_slot] = p_team;
	if (s.balance_candidate == int(p_slot)) {
		s.balance_candidate = -1;
	}
	if (s.balance_candidate < 0) {
		s.pick_balance_candidate();
	}
	return core::Status::success();
}

core::Status RoundNatives::player_leave(uint8_t p_slot, LogicRuntime &r_runtime) {
	(void)r_runtime;
	State &s = *state_;
	s.entered[p_slot] = false;
	s.team[p_slot] = 0;
	s.bot[p_slot] = false;
	if (s.balance_candidate == int(p_slot)) {
		s.balance_candidate = -1;
	}
	return core::Status::success();
}

core::Status RoundNatives::player_spawned(uint8_t p_slot, uint8_t p_team, LogicRuntime &r_runtime) {
	State &s = *state_;
	for (const uint32_t h : s.humans) {
		BF3_TRY(r_runtime.force_event(h, kOnPlayerSpawned, LogicContext{ int32_t(p_slot), p_team }));
	}
	return core::Status::success();
}

core::Status RoundNatives::player_died(uint8_t p_slot, uint8_t p_team, int p_killer, uint8_t p_killer_team, LogicRuntime &r_runtime) {
	State &s = *state_;
	for (const uint32_t h : s.humans) {
		BF3_TRY(r_runtime.force_event(h, kOnPlayerDied, LogicContext{ int32_t(p_slot), p_team }));
	}

	if (s.phase == RoundPhase::InRound && p_killer >= 0 && p_killer != int(p_slot) && (p_killer_team == 1 || p_killer_team == 2)) {
		const bool enemy = p_killer_team != p_team;
		const bool killer_bot = p_killer < int(kSlots) && s.bot[size_t(p_killer)];
		for (State::Counter &c : s.counters) {
			if (c.team != p_killer_team || (c.ignore_ai && killer_bot)) {
				continue;
			}
			c.kills += enemy ? c.enemy_weight : double(std::isfinite(s.config.team_kill_weight) ? s.config.team_kill_weight : 0.0f);
			c.kills = std::max(0.0, c.kills);
			BF3_TRY(s.publish_counter(c, r_runtime));
		}
	}

	if (s.balance_candidate == int(p_slot) && s.entered[p_slot]) {
		s.team[p_slot] = s.team[p_slot] == 1 ? 2 : 1;
		++s.moves;
		s.balance_candidate = -1;
	}
	if (s.balance_candidate < 0) {
		s.pick_balance_candidate();
	}
	return core::Status::success();
}

core::Status RoundNatives::kill_undone(uint8_t p_killer_team, LogicRuntime &r_runtime) {
	State &s = *state_;
	if (s.phase != RoundPhase::InRound) {
		return core::Status::success();
	}
	for (State::Counter &c : s.counters) {
		if (c.team == p_killer_team) {
			c.kills = std::max(0.0, c.kills - c.enemy_weight);
			BF3_TRY(s.publish_counter(c, r_runtime));
		}
	}
	return core::Status::success();
}

core::Status RoundNatives::tick_round(Tick p_tick, LogicRuntime &r_runtime) {
	State &s = *state_;
	s.tick = p_tick;
	if (s.phase != RoundPhase::PreRound) {
		return core::Status::success();
	}
	const uint32_t players = s.player_count();
	if (!s.counting && players >= min_players()) {
		const float seconds = s.config.preround_override_s >= 0.0f && std::isfinite(s.config.preround_override_s) ? s.config.preround_override_s
																												 : s.params.round_restart_countdown_s;
		s.counting = true;
		s.countdown_end = p_tick + logic_delay_ticks(std::min(seconds, 3600.0f));
	}
	if (s.counting && p_tick >= s.countdown_end) {
		s.counting = false;
		s.phase = RoundPhase::InRound;
		for (const uint32_t pr : s.pre_rounds) {
			s.note(pr, NativeTimelineEntry::What::RoundOver, 1);
			BF3_TRY(r_runtime.force_event(pr, kStartRound, LogicContext{}));
			BF3_TRY(r_runtime.force_event(pr, kEnablePlayerMovement, LogicContext{}));
		}
	}
	return core::Status::success();
}

RoundPhase RoundNatives::phase() const {
	return state_->phase;
}

uint8_t RoundNatives::winner() const {
	return state_->winner;
}

int RoundNatives::kills(uint8_t p_team) const {
	int64_t k = 0;
	for (const State::Counter &c : state_->counters) {
		if (c.team == p_team) {
			k += std::max<int64_t>(c.published, 0);
		}
	}
	return int(std::min<int64_t>(k, INT32_MAX));
}

int RoundNatives::kill_limit() const {
	const State &s = *state_;
	if (s.config.kill_limit_override > 0) {
		return s.config.kill_limit_override;
	}
	return s.counters.empty() ? 100 : s.counters.front().max_kill_count;
}

bool RoundNatives::input_restricted() const {
	return state_->restricted;
}

uint32_t RoundNatives::restriction_mask() const {
	return state_->params.restrict_allowed;
}

Tick RoundNatives::countdown_end_tick() const {
	return state_->counting ? state_->countdown_end : 0;
}

uint8_t RoundNatives::team_of(uint8_t p_slot) const {
	return state_->entered[p_slot] ? state_->team[p_slot] : 0;
}

bool RoundNatives::take_unspawn_all() {
	const bool f = state_->unspawn_flag;
	state_->unspawn_flag = false;
	return f;
}

bool RoundNatives::take_restart() {
	const bool f = state_->restart_flag;
	state_->restart_flag = false;
	return f;
}

bool RoundNatives::take_level_end() {
	const bool f = state_->level_end_flag;
	state_->level_end_flag = false;
	return f;
}

float RoundNatives::player_killed_delay_s() const {
	return state_->params.player_killed_delay_s;
}

bool RoundNatives::killer_camera() const {
	return state_->config.killer_camera;
}

std::span<const NativeTimelineEntry> RoundNatives::timeline() const {
	return state_->timeline;
}

uint32_t RoundNatives::balance_moves() const {
	return state_->moves;
}

bool RoundNatives::moves_at_death(uint8_t p_slot) const {
	return state_->balance_candidate == int(p_slot) && state_->entered[p_slot];
}

uint32_t RoundNatives::players() const {
	return state_->player_count();
}

uint32_t RoundNatives::min_players() const {
	const State &s = *state_;
	const int32_t n = s.config.min_players_override >= 0 ? s.config.min_players_override : s.params.round_min_players;
	return uint32_t(std::clamp(n, 0, 256));
}

uint32_t RoundNatives::players_needed() const {
	const State &s = *state_;
	if (s.phase != RoundPhase::PreRound || s.counting) {
		return 0;
	}
	const uint32_t players = s.player_count();
	return players >= min_players() ? 0 : min_players() - players;
}

const RoundNativeParams &RoundNatives::params() const {
	return state_->params;
}

std::span<const RoundNatives::NativeCoverage> RoundNatives::coverage() const {
	return state_->coverage;
}

}

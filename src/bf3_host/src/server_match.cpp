#include "server_match.h"

#include "bf3/host/bot_fill.h"

#include <cstdio>
#include <utility>

namespace bf3::host {

void MatchControl::configure(const GameServerConfig &p_config, size_t p_slots) {
	fixed_.ticks = p_config.respawn_ticks;
	slots_ = p_slots;
	deploy_screen_ = p_config.rules.deploy_screen;
	rules_flags_ = uint8_t(net::rules_flags::kTdm | (p_config.rules.deploy_screen ? net::rules_flags::kDeployScreen : 0u) |
			(p_config.rules.revive ? net::rules_flags::kRevive : 0u));
	logic_digest_ = p_config.level->logic_digest;
}

core::Status MatchControl::create_rules(const GameServerConfig &p_config, uint64_t p_match_seed) {
	core::Result<std::unique_ptr<TdmRules>> rules = TdmRules::create(*p_config.level, p_config.bots != nullptr ? p_config.bots->level : nullptr,
			p_config.rules, p_match_seed, uint32_t(slots_));
	if (!rules) {
		return core::error_at("GameServer", rules.error());
	}
	rules_ = std::move(*rules);
	return core::Status::success();
}

RespawnPolicy &MatchControl::policy() {
	if (rules_ != nullptr) {
		return *rules_;
	}
	return fixed_;
}

uint8_t MatchControl::connect(size_t p_slot, bool p_bot, std::string_view p_name) {
	if (!on() || p_slot >= slots_) {
		return 1;
	}
	return rules_->on_connect(uint8_t(p_slot), p_bot, p_name);
}

uint8_t MatchControl::carry(size_t p_slot, std::string_view p_name, uint8_t p_team, int p_squad_mate) {
	if (!on() || p_slot >= slots_) {
		return p_team;
	}
	return rules_->on_carry(uint8_t(p_slot), p_name, p_team, p_squad_mate);
}

void MatchControl::leave(size_t p_slot) {
	if (on() && p_slot < slots_) {
		rules_->on_disconnect(uint8_t(p_slot));
	}
}

bool MatchControl::waits_for_deploy(size_t p_slot) const {
	if (!on() || p_slot >= slots_) {
		return false;
	}
	return deploy_screen_ || rules_->team_of(uint8_t(p_slot)) == 0;
}

sim::Command MatchControl::restrict(const sim::Command &p_command) const {
	return on() ? rules_->apply_input_restriction(p_command) : p_command;
}

size_t MatchControl::choose_spawn(size_t p_slot, std::span<const sim::SpawnPoint> p_points, const ServerView &p_view, sim::Tick p_tick) const {
	if (!on() || p_slot >= slots_) {
		return SIZE_MAX;
	}
	return rules_->choose_spawn(uint8_t(p_slot), p_points, p_view, p_tick);
}

void MatchControl::request_spawn(size_t p_slot, uint8_t p_kit, uint8_t p_spawn_choice, sim::Tick p_tick) {
	(void)rules_->request_spawn(uint8_t(p_slot), p_kit, p_spawn_choice, p_tick);
}

void MatchControl::request_squad(size_t p_slot, const net::SquadRequest &p_request) {
	(void)rules_->request_squad(uint8_t(p_slot), p_request);
}

void MatchControl::request_suicide(size_t p_slot) {
	(void)rules_->request_suicide(uint8_t(p_slot));
}

bool MatchControl::take_suicide(size_t p_slot) {
	return on() && rules_->take_suicide(uint8_t(p_slot));
}

namespace {

bool same_order(const SquadOrderState &p_a, const SquadOrderState &p_b) {
	return p_a.kind == p_b.kind && p_a.area == p_b.area && p_a.position == p_b.position;
}

SquadOrderMessage order_message(uint8_t p_team, uint8_t p_squad, const SquadOrderState &p_order) {
	net::SquadOrder o;
	o.team = p_team;
	o.squad = p_squad;
	o.kind = p_order.kind;
	o.area = p_order.area;
	o.position = p_order.position;
	return SquadOrderMessage{ p_team, net::encode_squad_order(o) };
}

}

void MatchControl::describe(net::MatchInfo &r_message) const {
	if (!on()) {
		return;
	}
	r_message.phase = rules_->phase();
	r_message.logic_digest = logic_digest_;
	r_message.rules_flags = rules_flags_;
}

MatchBroadcast MatchControl::join_messages(sim::Tick p_tick, uint8_t p_team) const {
	MatchBroadcast b;
	if (!on()) {
		return b;
	}
	b.state = net::encode_match_state(rules_->match_state());
	size_t clamped = 0;
	b.players = net::encode_player_list(rules_->player_list(p_tick, true), &clamped);
	for (uint8_t q = 1; p_team >= 1 && p_team <= kSquadTeams && q <= net::kMaxSquads; ++q) {
		const SquadOrderState &o = rules_->squads().order(p_team, q);
		if (o.kind != net::SquadOrderKind::None) {
			b.orders.push_back(order_message(p_team, q, o));
		}
	}
	return b;
}

MatchBroadcast MatchControl::take_broadcast(sim::Tick p_tick) {
	MatchBroadcast b;
	if (!on()) {
		return b;
	}
	if (rules_->squads().revision() != orders_revision_) {
		orders_revision_ = rules_->squads().revision();
		for (uint8_t t = 1; t <= kSquadTeams; ++t) {
			for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
				const SquadOrderState &o = rules_->squads().order(t, q);
				SquadOrderState &sent = sent_orders_[t - 1u][q - 1u];
				if (!same_order(o, sent)) {
					sent = o;
					b.orders.push_back(order_message(t, q, o));
				}
			}
		}
	}
	const bool state_changed = rules_->match_state_changed();
	const bool list_due = rules_->players_changed() && (!player_list_sent_ || p_tick - player_list_tick_ >= net::kPlayerListIntervalTicks);
	if (!state_changed && !list_due) {
		return b;
	}
	if (state_changed) {
		b.state = net::encode_match_state(rules_->match_state());
	}
	if (list_due) {
		size_t clamped = 0;
		b.players = net::encode_player_list(rules_->player_list(p_tick, false), &clamped);
		player_list_tick_ = p_tick;
		player_list_sent_ = true;
	}
	if (state_changed) {
		rules_->mark_state_sent();
	}
	if (list_due) {
		rules_->mark_players_sent();
	}
	return b;
}

void MatchControl::step(sim::Tick p_tick, const ServerView &p_view, const Gunplay &p_gunplay, std::span<sim::SoldierState> r_states) {
	orders_.clear();
	if (rules_ == nullptr) {
		return;
	}
	const core::Status st = rules_->step(p_tick, p_view, p_gunplay, r_states, orders_);
	if (!st.ok() && step_failures_++ == 0) {
		std::fprintf(stderr, "GameServer: tdm rules: %s\n", st.error().message.c_str());
	}
}

std::vector<RulesOrder> MatchControl::take_orders() {
	std::vector<RulesOrder> orders = std::move(orders_);
	orders_.clear();
	return orders;
}

}

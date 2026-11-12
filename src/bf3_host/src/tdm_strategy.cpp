#include "tdm_strategy.h"

#include "ai_world.h"
#include "tdm_bot_shared.h"

#include "bf3/host/game_server.h"
#include "bf3/host/server_view.h"
#include "bf3/host/squads.h"
#include "bf3/host/tdm_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

net::SquadOrderKind wire_order(ai::SquadOrder p_order) {
	switch (p_order) {
		case ai::SquadOrder::Attack:
			return net::SquadOrderKind::Attack;
		case ai::SquadOrder::Defend:
			return net::SquadOrderKind::Defend;
		case ai::SquadOrder::Move:
			return net::SquadOrderKind::Move;
		case ai::SquadOrder::None:
			break;
	}
	return net::SquadOrderKind::None;
}

}

ControlPointState control_point_state(const sim::logic::ConquestPointView &p_point) {
	ControlPointState state;
	state.owner = p_point.controlled ? int(p_point.team) : 0;
	state.take_over = p_point.controlled ? std::clamp(p_point.progress / sim::logic::kFlagTop, 0.0f, 1.0f) : 1.0f;
	return state;
}

std::vector<int> match_control_points(
		std::span<const BotStrategicArea> p_areas, std::span<const sim::logic::ConquestPointView> p_points) {
	std::vector<int> matched(p_areas.size(), -1);
	for (size_t i = 0; i < p_areas.size(); ++i) {
		const BotStrategicArea &area = p_areas[i];
		if ((area.kind != BotStrategicArea::Kind::Flag && area.kind != BotStrategicArea::Kind::Hq) ||
				area.level_object == UINT32_MAX) {
			continue;
		}
		for (size_t p = 0; p < p_points.size(); ++p) {
			if (p_points[p].level_object == area.level_object) {
				matched[i] = int(p);
				break;
			}
		}
	}
	return matched;
}

TdmStrategy::TdmStrategy(GameServer &r_server, TdmShared &r_shared) : server_(r_server), shared_(r_shared) {}

TdmStrategy::~TdmStrategy() = default;

core::Result<std::unique_ptr<TdmStrategy>> TdmStrategy::create(GameServer &r_server, TdmShared &r_shared) {
	TdmRules *rules = r_server.rules();
	const std::shared_ptr<const BotLevel> &level = r_shared.host.nav;
	if (rules == nullptr || level == nullptr || level->nav == nullptr || level->clusters == nullptr) {
		return core::make_error("tdm strategy: needs the TDM rules and a bot level with nav and clusters");
	}
	std::unique_ptr<TdmStrategy> s(new TdmStrategy(r_server, r_shared));
	s->rules_ = rules;
	s->tuning_ = r_shared.config->strategy;
	s->settings_.max_bots = int32_t(net::kMaxPlayerRows);
	AiWorldConfig config;
	config.level = level;
	config.world = &r_server.view().world();
	config.squad_size = rules->squads().squad_size();
	config.seed = r_shared.host.match_seed;
	for (size_t i = 0; i < 3; ++i) {
		config.eye_heights[i] = r_shared.host.eye_heights[i];
	}
	core::Result<std::unique_ptr<HostAiWorld>> world = HostAiWorld::create(config);
	if (!world) {
		return core::error_at("tdm strategy", world.error());
	}
	s->world_ = std::move(*world);
	s->service_ = std::make_unique<RulesSquadService>(*rules);
	ai::SquadServices squad_services;
	squad_services.world = s->world_.get();
	squad_services.squads = s->service_.get();
	squad_services.strategy = s.get();
	squad_services.information = &s->world_->information();
	s->squads_ = std::make_unique<ai::SquadManager>(squad_services);
	s->decisions_ = std::make_unique<ai::StrategyDecisions>(
			s->manager_, *s->squads_, s->world_->strategic(), s.get(), s->tuning_);
	if (rules->natives().conquest() != nullptr) {
		s->command_ = std::make_unique<ai::SquadCommanderGame>(*s->squads_, *s->decisions_);
		s->commander_ = std::make_unique<ai::BFCommander>(*s->command_);
	}
	for (int side = 1; side <= 2; ++side) {
		ai::SaiServices services;
		services.timer = s.get();
		services.settings = &s->settings_;
		services.game = s.get();
		services.objects = &s->world_->strategic();
		services.information = &s->world_->information();
		services.grid = &s->world_->grid(side);
		services.squads = s->squads_.get();
		services.decisions = s->decisions_.get();
		services.commander = s->commander_.get();
		s->sai_[size_t(side - 1)] = std::make_unique<ai::Sai>(side, services);
		s->next_cycle_[size_t(side - 1)] = sim::Tick(side - 1) * (s->tuning_.sai_period_ticks / 2u);
	}
	s->orders_ = std::make_unique<ai::SquadOrders>(
			*s->squads_, s.get(), r_shared.config->behaviours, squad_services.formation.max_wait_s);
	s->random_ = std::make_unique<ai::AIRandom>(ai::AIRandom::seed_for(r_shared.host.match_seed, 0x5357u));
	s->random_->init();
	ai::SpawnServices spawn_services;
	spawn_services.squads = s->squads_.get();
	spawn_services.random = s->random_.get();
	spawn_services.information = &s->world_->information();
	s->spawner_ = std::make_unique<ai::BotSpawner>(spawn_services);
	s->heat_.assign(level->areas.size(), 0.0f);
	return s;
}

void TdmStrategy::add_bot(uint8_t p_slot, ai::BotMain &r_bot) {
	if (p_slot >= bots_.size() || bots_[p_slot] != nullptr) {
		return;
	}
	world_->add_bot(p_slot, false);
	if (!shared_.config->squads) {
		return;
	}
	world_->set_squad_bot(p_slot, &r_bot);
	bots_[p_slot] = std::make_unique<ai::BotSquad>(r_bot, *world_, service_.get(), tuning_);
	orders_->add_bot(p_slot, *bots_[p_slot]);
	world_->step(server_.view(), shared_.heard, shared_.firing, server_.current_tick());
	sync_squads();
	draw_spawn(p_slot);
}

void TdmStrategy::remove_bot(uint8_t p_slot) {
	if (p_slot >= bots_.size() || bots_[p_slot] == nullptr) {
		return;
	}
	orders_->remove_bot(p_slot);
	for (int side = 1; side <= 2; ++side) {
		if (ai::SquadMember *const member = squads_->squad_member(p_slot, side)) {
			member->detach_bot();
		}
	}
	world_->set_squad_bot(p_slot, nullptr);
	bots_[p_slot].reset();
	spawn_life_[p_slot] = 0;
}

std::optional<sim::Vec3> TdmStrategy::mission(uint8_t p_slot) const {
	if (p_slot >= bots_.size() || bots_[p_slot] == nullptr || bots_[p_slot]->has_no_order()) {
		return std::nullopt;
	}
	for (int side = 1; side <= 2; ++side) {
		const ai::SquadCoordinator *squad = squads_->squad_from_player(p_slot, side);
		if (squad != nullptr && squad->mission_status() == ai::SquadMissionStatus::Accepted) {
			return squad->squad_goto_position();
		}
	}
	return std::nullopt;
}

uint64_t TdmStrategy::squad_mates(uint8_t p_slot) const {
	const SquadTable &table = rules_->squads();
	uint64_t mates = 0;
	for (const uint8_t m : table.members(table.team_of(p_slot), table.squad_of(p_slot))) {
		mates |= m < 64 && m != p_slot ? uint64_t(1) << m : 0u;
	}
	return mates;
}

float TdmStrategy::area_heat(const ai::AIStrategicArea &p_area) const {
	for (size_t i = 0; i < heat_.size(); ++i) {
		if (world_->area(i) == &p_area) {
			return heat_[i];
		}
	}
	return 0.0f;
}

void TdmStrategy::squad_order(int p_side, int p_squad, ai::SquadOrder p_order, int p_area, const ai::Vec3 &p_position) {
	SquadOrderState order;
	order.kind = wire_order(p_order);
	order.area = p_area >= 0 && p_area < int(net::kNoSquadArea) ? uint16_t(p_area) : net::kNoSquadArea;
	order.position = p_position;
	(void)server_.set_squad_order(uint8_t(p_side), uint8_t(p_squad), order);
	shared_.count(&TdmBotStats::squad_orders, uint64_t(p_order != ai::SquadOrder::None ? 1u : 0u));
}

int TdmStrategy::tickets(int p_side) const {
	if (const sim::logic::ConquestNatives *conquest = rules_->natives().conquest()) {
		return int(std::clamp<int64_t>(conquest->tickets(uint8_t(p_side)), 0, INT32_MAX));
	}
	const net::MatchState &ms = rules_->match_state();
	const int enemy = p_side == 1 ? 1 : 0;
	return std::max(0, int(ms.kill_limit) - int(ms.kills[size_t(enemy)]));
}

int TdmStrategy::number_of_control_points(int p_side) const {
	return world_->number_of_control_points(p_side);
}

int TdmStrategy::spawn_kit(int p_player_id) const {
	const ai::SquadPlayer *player = world_->player(p_player_id);
	return player != nullptr ? player->spawn_kit() : ai::kDeadBotKit;
}

bool TdmStrategy::is_player_commander(int) const {
	return false;
}

bool TdmStrategy::own_all_takeable_control_points(int p_side) const {
	return sai_[0] != nullptr && sai_[0]->own_all_takeable_control_points(p_side);
}

bool TdmStrategy::have_any_owned_and_takeable_control_points(int p_side) const {
	return sai_[0] != nullptr && sai_[0]->have_any_owned_and_takeable_control_points(p_side);
}

void TdmStrategy::sync_squads() {
	const SquadTable &table = rules_->squads();
	if (table.revision() == revision_) {
		return;
	}
	revision_ = table.revision();
	for (int side = 1; side <= 2; ++side) {
		for (int q = 1; q <= squads_->coordinator_count(side); ++q) {
			ai::SquadCoordinator &squad = squads_->coordinator(side, q);
			const std::span<const uint8_t> wanted = table.members(uint8_t(side), uint8_t(q));
			for (int i = squad.number_of_squad_members() - 1; i >= 0; --i) {
				const int id = squad.squad_member(i)->player_id();
				if (std::find(wanted.begin(), wanted.end(), uint8_t(id)) != wanted.end()) {
					continue;
				}
				squad.remove_squad_member(id);
				if (size_t(id) < bots_.size() && bots_[size_t(id)] != nullptr) {
					bots_[size_t(id)]->clear_order();
					bots_[size_t(id)]->bot().set_squad_goal(ai::SquadGoal{});
				}
			}
		}
	}
	for (int side = 1; side <= 2; ++side) {
		for (int q = 1; q <= squads_->coordinator_count(side); ++q) {
			for (const uint8_t slot : table.members(uint8_t(side), uint8_t(q))) {
				if (squads_->squad_member(slot, side) == nullptr) {
					squads_->player_joins_squad(slot, q, side);
					if (slot < bots_.size() && bots_[slot] != nullptr) {
						bots_[slot]->join_squad(q);
					}
				}
			}
			squads_->new_squad_leader(side, q);
		}
	}
}

void TdmStrategy::sync_control_points() {
	const sim::logic::ConquestNatives *conquest = rules_->natives().conquest();
	if (conquest == nullptr) {
		return;
	}
	const std::span<const sim::logic::ConquestPointView> points = conquest->points();
	const std::vector<BotStrategicArea> &areas = shared_.host.nav->areas;
	if (matched_points_ != points.size() || control_points_.size() != areas.size()) {
		control_points_ = match_control_points(areas, points);
		matched_points_ = points.size();
	}
	for (size_t i = 0; i < control_points_.size(); ++i) {
		if (control_points_[i] >= 0) {
			const ControlPointState state = control_point_state(points[size_t(control_points_[i])]);
			world_->set_control_point(i, state.owner, state.take_over);
		}
	}
}

void TdmStrategy::draw_spawn(uint8_t p_slot) {
	const std::span<const sim::SoldierState> states = server_.view().states();
	if (p_slot >= states.size()) {
		return;
	}
	spawn_life_[p_slot] = states[p_slot].life + 1u;
	orders_->deployed(p_slot);
	const SquadTable &table = rules_->squads();
	const uint8_t team = table.team_of(p_slot);
	const ai::SquadMember *const member = team != 0 ? squads_->squad_member(int(p_slot), team) : nullptr;
	uint8_t choice = 0;
	if (spawner_->follows_squad(member, tuning_.squad_spawn_draw)) {
		const ai::SquadMember *const mate = spawner_->squad_mate_to_follow(member);
		choice = 1;
		for (uint8_t i = 0; mate != nullptr && i < table.squad_size(); ++i) {
			if (table.mate(p_slot, i) == mate->player_id()) {
				choice = uint8_t(i + 1u);
				break;
			}
		}
	}
	rules_->set_spawn_choice(p_slot, choice);
}

void TdmStrategy::choose_spawns() {
	const std::span<const sim::SoldierState> states = server_.view().states();
	for (size_t slot = 0; slot < bots_.size() && slot < states.size(); ++slot) {
		if (bots_[slot] != nullptr && spawn_life_[slot] != states[slot].life + 1u) {
			draw_spawn(uint8_t(slot));
		}
	}
}

void TdmStrategy::step(sim::Tick p_tick) {
	if (stepped_ == p_tick) {
		return;
	}
	stepped_ = p_tick;
	tick_ = p_tick;
	TdmShared &sh = shared_;
	world_->step(server_.view(), sh.heard, sh.firing, p_tick);
	sync_control_points();
	sync_squads();
	choose_spawns();
	for (uint32_t i = 0; i < tuning_.squad_updates_per_tick; ++i) {
		squads_->update(p_tick);
	}
	for (int side = 1; side <= 2; ++side) {
		ai::Sai &sai = *sai_[size_t(side - 1)];
		sim::Tick &next = next_cycle_[size_t(side - 1)];
		if (sai.stage() == ai::SaiStage::Done) {
			if (p_tick < next) {
				continue;
			}
			next = p_tick + tuning_.sai_period_ticks;
			if (heat_tick_ != p_tick) {
				heat_tick_ = p_tick;
				for (size_t i = 0; i < heat_.size(); ++i) {
					heat_[i] = tdm_area_temperature(sh, sh.host.nav->areas[i], p_tick);
				}
			}
		}
		sai.update(tick_seconds());
	}
	ai::SquadCohesion cohesion;
	orders_->update(p_tick, kSquadNearMetres, cohesion);
	sh.count(&TdmBotStats::squad_samples, cohesion.samples);
	sh.count(&TdmBotStats::squad_near, cohesion.near);
	if (sh.stats != nullptr) {
		for (size_t b = 0; b < cohesion.metres.size(); ++b) {
			if (cohesion.metres[b] != 0) {
				sh.stats->squad_metres[b].fetch_add(cohesion.metres[b], std::memory_order_relaxed);
			}
		}
	}
}

}

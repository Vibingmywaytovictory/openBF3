#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/squad/bot_squad.h"
#include "bf3/ai/squad/squad_manager.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

namespace {

constexpr uint8_t kNoOrderedBehaviour = 0xFF;

SquadOrder table_order(const SquadCoordinator &p_squad) {
	return p_squad.mission_status() == SquadMissionStatus::Accepted ? p_squad.order() : SquadOrder::None;
}

bool live_position(const SquadPlayer &p_player, Vec3 &r_position) {
	const AiObject *const body = p_player.is_alive() && !p_player.is_man_down() ? p_player.current_vehicle() : nullptr;
	if (body == nullptr) {
		return false;
	}
	r_position = body->position();
	return std::isfinite(r_position.x) && std::isfinite(r_position.y) && std::isfinite(r_position.z);
}

}

BotSquad::BotSquad(BotMain &r_owner, SquadWorld &r_world, SquadService *p_squads, const StrategyTuning &p_tuning) :
		bot_(&r_owner), world_(&r_world), squads_(p_squads), tuning_(&p_tuning) {
	bot_->set_taking_orders(true);
}

BotSquad::~BotSquad() = default;

bool BotSquad::join_squad(int p_squad) {
	const SquadPlayer *const player = world_->player(bot_->player_id());
	if (player == nullptr || squads_ == nullptr) {
		return false;
	}
	const bool joined = squads_->join_squad(p_squad, *player);
	if (joined) {
		squad_id_ = p_squad;
		bot_->join_squad(p_squad);
	}
	return joined;
}

bool BotSquad::is_squad_leader() const {
	const SquadPlayer *const player = world_->player(bot_->player_id());
	return player != nullptr && squads_ != nullptr && squad_id_ != 0 && squads_->is_squad_leader(squad_id_, *player);
}

bool BotSquad::able() const {
	const SquadPlayer *const player = world_->player(bot_->player_id());
	return player != nullptr && player->is_alive() && !player->is_man_down();
}

void BotSquad::apply(SquadOrder p_order, bool p_leading, const float *p_weights) {
	order_ = p_order;
	leading_ = p_leading;
	const uint8_t behaviour = p_weights != nullptr ? uint8_t(InfantryBehaviour::MoveTo) : kNoOrderedBehaviour;
	if (applied_ && weights_ == p_weights && bot_->ordered_behaviour() == int(int8_t(behaviour))) {
		return;
	}
	weights_ = p_weights;
	applied_ = true;
	bot_->receive_order(BotOrder(p_weights, behaviour));
}

void BotSquad::issue(const SquadCoordinator &p_squad, SquadOrder p_order, SquadOrderSink *p_sink) {
	lead_squad(p_order);
	if (p_sink != nullptr) {
		const SquadArea *area = p_squad.squad_strategic_area_assignment();
		p_sink->squad_order(p_squad.side() + 1, p_squad.squad_id() + 1, p_order, area != nullptr ? area->index() : -1,
				p_squad.squad_goto_position());
	}
}

void BotSquad::issue_squad_attack_order(const SquadCoordinator &p_squad, SquadOrderSink *p_sink) {
	issue(p_squad, SquadOrder::Attack, p_sink);
}

void BotSquad::issue_squad_move_order(const SquadCoordinator &p_squad, SquadOrderSink *p_sink) {
	issue(p_squad, SquadOrder::Move, p_sink);
}

void BotSquad::issue_squad_defend_order(const SquadCoordinator &p_squad, SquadOrderSink *p_sink) {
	issue(p_squad, SquadOrder::Defend, p_sink);
}

void BotSquad::recieve_attack_squad_order() {
	apply(SquadOrder::Attack, false, tuning_->order.data());
}

void BotSquad::recieve_defend_squad_order() {
	apply(SquadOrder::Defend, false, tuning_->order.data());
}

void BotSquad::recieve_move_squad_order() {
	apply(SquadOrder::Move, false, tuning_->order.data());
}

void BotSquad::follow_squad() {
	apply(SquadOrder::None, false, tuning_->order.data());
}

void BotSquad::lead_squad(SquadOrder p_order) {
	apply(p_order, true, p_order != SquadOrder::None ? tuning_->order.data() : nullptr);
}

void BotSquad::clear_order() {
	apply(SquadOrder::None, false, nullptr);
}

void BotSquad::recieve_attack_commander_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink) {
	if (able() && r_squad.order() == SquadOrder::Attack) {
		accept_squad_order(r_squad, p_sink);
	} else {
		decline_squad_order(r_squad);
	}
}

void BotSquad::recieve_defend_commander_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink) {
	if (able() && r_squad.order() == SquadOrder::Defend) {
		accept_squad_order(r_squad, p_sink);
	} else {
		decline_squad_order(r_squad);
	}
}

void BotSquad::recieve_move_commander_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink) {
	if (able() && r_squad.order() == SquadOrder::Move) {
		accept_squad_order(r_squad, p_sink);
	} else {
		decline_squad_order(r_squad);
	}
}

void BotSquad::accept_squad_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink) {
	const SquadOrder order = r_squad.order();
	r_squad.assign_squad(r_squad.squad_strategic_area_assignment(), r_squad.squad_goto_position(), order,
			SquadMissionStatus::Accepted);
	switch (order) {
		case SquadOrder::Attack:
			issue_squad_attack_order(r_squad, p_sink);
			break;
		case SquadOrder::Defend:
			issue_squad_defend_order(r_squad, p_sink);
			break;
		case SquadOrder::Move:
			issue_squad_move_order(r_squad, p_sink);
			break;
		case SquadOrder::None:
			break;
	}
}

void BotSquad::decline_squad_order(SquadCoordinator &r_squad) {
	r_squad.un_assign_squad();
}

SquadOrders::SquadOrders(
		SquadManager &r_squads, SquadOrderSink *p_sink, const BehaviourTuning &p_behaviour, float p_wait_s) :
		squads_(r_squads), sink_(p_sink), behaviour_(p_behaviour), wait_ticks_(sim::seconds_to_ticks(p_wait_s)) {
	for (int side = 1; side <= 2; ++side) {
		sent_[size_t(side - 1)].assign(size_t(squads_.coordinator_count(side)), SquadOrder::None);
		arrived_[size_t(side - 1)].assign(size_t(squads_.coordinator_count(side)), 0);
	}
}

void SquadOrders::add_bot(int p_player_id, BotSquad &r_squad) {
	if (p_player_id >= 0 && size_t(p_player_id) < bots_.size()) {
		bots_[size_t(p_player_id)] = &r_squad;
	}
}

void SquadOrders::remove_bot(int p_player_id) {
	if (p_player_id >= 0 && size_t(p_player_id) < bots_.size()) {
		bots_[size_t(p_player_id)] = nullptr;
	}
}

BotSquad *SquadOrders::bot_squad(int p_player_id) const {
	return p_player_id >= 0 && size_t(p_player_id) < bots_.size() ? bots_[size_t(p_player_id)] : nullptr;
}

bool SquadOrders::arrived(const SquadCoordinator &p_squad, sim::Tick p_tick, sim::Tick &r_since) const {
	const SquadMember *const leader = p_squad.squad_leader();
	Vec3 at{};
	const float radius = behaviour_.mission_arrive_m;
	if (leader == nullptr || !live_position(leader->player(), at) ||
			distance_sq(at, p_squad.squad_goto_position()) > radius * radius) {
		r_since = 0;
		return false;
	}
	if (r_since == 0) {
		r_since = std::max<sim::Tick>(p_tick, 1);
	}
	for (int i = 0; i < p_squad.number_of_squad_members(); ++i) {
		Vec3 member{};
		if (live_position(p_squad.squad_member(i)->player(), member) &&
				distance_sq(member, p_squad.squad_goto_position()) > radius * radius) {
			return p_tick >= r_since + wait_ticks_;
		}
	}
	return true;
}

void SquadOrders::pass_down(const SquadCoordinator &p_squad, SquadOrder p_order) {
	const SquadMember *const leader = p_squad.squad_leader();
	const int anchor = p_squad.formation().squad_member_index_at_slot(0);
	for (int i = 0; i < p_squad.number_of_squad_members(); ++i) {
		const SquadMember *const member = p_squad.squad_member(i);
		BotSquad *const bot = bot_squad(member->player_id());
		if (bot == nullptr) {
			continue;
		}
		if (member == leader || (i == anchor && (leader == nullptr || !leader->player().is_alive()))) {
			bot->lead_squad(p_order);
			continue;
		}
		switch (p_order) {
			case SquadOrder::Attack:
				bot->recieve_attack_squad_order();
				break;
			case SquadOrder::Defend:
				bot->recieve_defend_squad_order();
				break;
			case SquadOrder::Move:
				bot->recieve_move_squad_order();
				break;
			case SquadOrder::None:
				bot->follow_squad();
				break;
		}
	}
}

void SquadOrders::gather(const SquadCoordinator &p_squad) const {
	const SquadMember *const leader = p_squad.squad_leader();
	SquadGoal goal;
	goal.kind = SquadGoalKind::SquadTarget;
	goal.position = p_squad.squad_goto_position();
	for (int i = 0; i < p_squad.number_of_squad_members(); ++i) {
		const SquadMember *const member = p_squad.squad_member(i);
		BotSquad *const bot = member != leader ? bot_squad(member->player_id()) : nullptr;
		if (bot != nullptr) {
			bot->bot().set_squad_goal(goal);
		}
	}
}

void SquadOrders::deployed(int p_player_id) {
	if (p_player_id >= 0 && size_t(p_player_id) < origin_known_.size()) {
		origin_known_[size_t(p_player_id)] = false;
	}
}

bool SquadOrders::left_origin(int p_player_id, const Vec3 &p_position, float p_metres) {
	if (p_player_id < 0 || size_t(p_player_id) >= origin_known_.size()) {
		return false;
	}
	const size_t id = size_t(p_player_id);
	if (!origin_known_[id]) {
		origin_known_[id] = true;
		origin_[id] = p_position;
	}
	return distance_sq(p_position, origin_[id]) > p_metres * p_metres;
}

void SquadOrders::sample(const SquadCoordinator &p_squad, float p_near_m, SquadCohesion &r_cohesion) {
	const SquadMember *const leader = p_squad.squad_leader();
	if (leader != nullptr && !leader->player().is_alive()) {
		deployed(leader->player_id());
	}
	Vec3 lead{};
	if (leader == nullptr || !leader->player().is_alive() || !live_position(leader->player(), lead) ||
			!left_origin(leader->player_id(), lead, p_near_m)) {
		return;
	}
	for (int i = 0; i < p_squad.number_of_squad_members(); ++i) {
		const SquadMember *const member = p_squad.squad_member(i);
		Vec3 at{};
		if (member == leader || !member->player().is_alive() || !live_position(member->player(), at)) {
			continue;
		}
		const BotSquad *const bot = bot_squad(member->player_id());
		if (bot != nullptr && bot->bot().activity() == InfantryActivity::Fight) {
			continue;
		}
		const float d = distance(at, lead);
		++r_cohesion.samples;
		r_cohesion.near += d <= p_near_m ? 1u : 0u;
		++r_cohesion.metres[std::min(size_t(d), kCohesionBins - 1)];
	}
}

void SquadOrders::update(sim::Tick p_tick, float p_near_m, SquadCohesion &r_cohesion) {
	for (int side = 1; side <= 2; ++side) {
		for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
			SquadCoordinator &squad = squads_.coordinator(side, q);
			const SquadMember *const leader = squad.squad_leader();
			BotSquad *const lead = leader != nullptr && leader->is_bot() ? bot_squad(leader->player_id()) : nullptr;
			sim::Tick &since = arrived_[size_t(side - 1)][size_t(q - 1)];
			bool issued = false;
			if (squad.mission_status() == SquadMissionStatus::Issued) {
				++stats_.issued;
				if (lead == nullptr) {
					squad.un_assign_squad();
				} else if (squad.order() == SquadOrder::Attack) {
					lead->recieve_attack_commander_order(squad, sink_);
				} else if (squad.order() == SquadOrder::Defend) {
					lead->recieve_defend_commander_order(squad, sink_);
				} else {
					lead->recieve_move_commander_order(squad, sink_);
				}
				issued = squad.mission_status() == SquadMissionStatus::Accepted;
				++(issued ? stats_.accepted : stats_.declined);
				since = 0;
			} else if (squad.mission_status() == SquadMissionStatus::Accepted &&
					(lead == nullptr || arrived(squad, p_tick, since))) {
				squad.un_assign_squad();
				++stats_.completed;
				since = 0;
			}
			const SquadOrder order = squad.number_of_squad_members() > 0 ? table_order(squad) : SquadOrder::None;
			SquadOrder &sent = sent_[size_t(side - 1)][size_t(q - 1)];
			if (order != sent) {
				sent = order;
				if (sink_ != nullptr && !issued) {
					const SquadArea *area = squad.squad_strategic_area_assignment();
					sink_->squad_order(
							side, q, order, area != nullptr ? area->index() : -1, squad.squad_goto_position());
				}
				stats_.passed_down += order != SquadOrder::None ? 1u : 0u;
			}
			if (squad.number_of_squad_members() == 0) {
				continue;
			}
			pass_down(squad, order);
			if (order != SquadOrder::None && since != 0) {
				gather(squad);
			}
			sample(squad, p_near_m, r_cohesion);
		}
	}
}

}

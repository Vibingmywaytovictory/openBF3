#include "bf3/ai/bot/bot_main.h"

#include <algorithm>
#include <vector>

namespace bf3::ai {

BotMain::BotMain(uint32_t p_bot_id, int p_player_id, int p_side, InfantryBody &r_body, const BehaviourTuning &p_tuning,
		const float *p_personality) :
		bot_id_(p_bot_id), player_id_(p_player_id), side_(p_side), body_(r_body), tuning_(p_tuning), plan_(r_body) {
	for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
		const BehaviourCurve &c = tuning_.curves[i];
		curves_[i] = std::make_unique<UCLinear>(c.slope, c.offset);
		basic_weights_[i] = c.weight;
	}
	const auto curve = [&](InfantryBehaviour p_kind) -> const UrgeCurve & { return *curves_[size_t(p_kind)]; };
	behaviours_[size_t(InfantryBehaviour::Revive)] =
			std::make_unique<BBRevive>(curve(InfantryBehaviour::Revive), body_);
	behaviours_[size_t(InfantryBehaviour::FireInfantery)] =
			std::make_unique<BBFireInfantery>(curve(InfantryBehaviour::FireInfantery), body_);
	behaviours_[size_t(InfantryBehaviour::TakeCoverInfantry)] =
			std::make_unique<BBTakeCoverInfantry>(curve(InfantryBehaviour::TakeCoverInfantry), body_);
	behaviours_[size_t(InfantryBehaviour::Avoid)] = std::make_unique<BBAvoid>(curve(InfantryBehaviour::Avoid), body_);
	behaviours_[size_t(InfantryBehaviour::MoveTo)] = std::make_unique<BBMoveTo>(
			curve(InfantryBehaviour::MoveTo), body_, squad_goal_, tuning_, ordered_behaviour_);
	behaviours_[size_t(InfantryBehaviour::Random)] =
			std::make_unique<BBRandom>(curve(InfantryBehaviour::Random), body_);
	behaviours_[size_t(InfantryBehaviour::Idle)] = std::make_unique<BBIdle>(curve(InfantryBehaviour::Idle), body_);
	std::vector<UrgencyGenerator *> generators;
	for (const std::unique_ptr<BotBehaviour> &b : behaviours_) {
		generators.push_back(b.get());
	}
	merger_ = std::make_unique<UrgencyMerger>(
			int(kInfantryBehaviourCount), behaviour_urgencies_.data(), std::move(generators));
	set_personality(p_personality);
	reset();
}

BotMain::~BotMain() = default;

void BotMain::set_personality(const float *p_weights) {
	for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
		personality_[i] = p_weights != nullptr ? p_weights[i] : 1.0f;
	}
	calculate_mods();
}

void BotMain::calculate_mods() {
	for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
		if (taking_orders_ && current_orders_ != nullptr) {
			behaviour_urgencies_[i] = current_orders_[i] * basic_weights_[i] * personality_[i];
		} else {
			behaviour_urgencies_[i] = basic_weights_[i] * personality_[i];
		}
	}
}

bool BotMain::receive_order(const BotOrder &p_order) {
	if (!taking_orders_) {
		return false;
	}
	current_orders_ = p_order.weights;
	ordered_behaviour_ = int(int8_t(p_order.behaviour));
	calculate_mods();
	return true;
}

void BotMain::reset() {
	for (const std::unique_ptr<BotBehaviour> &b : behaviours_) {
		b->reset(this);
	}
	merger_->reset(this);
	current_ = nullptr;
	chosen_urgency_ = 0.0f;
	plan_.clear(InfantryActivity::Idle);
	plan_invalid_ = true;
	decided_senses_ = InfantrySenses{};
	ordered_behaviour_ = -1;
}

void BotMain::update_bot(bool p_alive, bool p_new_life) {
	alive_ = p_alive;
	if (p_new_life) {
		reset();
	}
}

void BotMain::see(double) {
	body_.see();
}

void BotMain::update_sensing(double) {
	body_.update_sensing();
}

bool BotMain::wants_decision() const {
	if (plan_invalid_ || current_ == nullptr || body_.senses() != decided_senses_) {
		return true;
	}
	const uint32_t period = std::max<uint32_t>(tuning_.decision_ticks, 1u);
	return (body_.tick() + uint32_t(std::max(player_id_, 0))) % period == 0;
}

int BotMain::decision_making(double) {
	const sim::Tick tick = body_.tick();
	const bool unplanned = plan_invalid_ || current_ == nullptr || body_.senses() != decided_senses_;
	++stats_.decisions;
	++(unplanned ? stats_.unplanned : stats_.planned);
	calculate_mods();
	for (size_t attempt = 0; attempt <= kInfantryBehaviourCount; ++attempt) {
		float urgency = merger_->urgency(this);
		BotBehaviour *winner = static_cast<BotBehaviour *>(merger_->chosen_generator());
		if (winner == nullptr || !(urgency > 0.0f)) {
			winner = behaviours_[size_t(InfantryBehaviour::Idle)].get();
			urgency = 0.0f;
		}
		const bool was_active = winner == current_ && !plan_invalid_;
		if (winner->plan(plan_, was_active)) {
			if (current_ != nullptr && current_ != winner) {
				current_->deactivate();
			}
			current_ = winner;
			chosen_urgency_ = urgency;
			plan_invalid_ = false;
			++stats_.chosen[size_t(winner->kind())];
			break;
		}
		winner->refuse(tick);
		++stats_.refusals;
	}
	decided_senses_ = body_.senses();
	decided_tick_ = tick;
	return 1;
}

void BotMain::plan_execution(double) {
	if (plan_invalid_) {
		return;
	}
	body_.begin_plan(plan_.activity());
	const bool ok = plan_.execute(this);
	body_.end_plan();
	if (!ok) {
		plan_invalid_ = true;
		++stats_.plan_failures;
	}
}

void BotMain::update_pathfinding(bool &r_first, bool *r_second) {
	r_first = false;
	if (r_second != nullptr) {
		*r_second = false;
	}
}

bool BotMain::join_squad(int p_squad) {
	squad_ = p_squad;
	return true;
}

InfantryBehaviour BotMain::behaviour() const {
	return current_ != nullptr ? current_->kind() : InfantryBehaviour::Idle;
}

float BotMain::behaviour_urgency(InfantryBehaviour p_behaviour) const {
	return behaviours_[size_t(p_behaviour)]->situation_value() > 0.0f
			? curves_[size_t(p_behaviour)]->calculate(behaviours_[size_t(p_behaviour)]->situation_value())
			: 0.0f;
}

float BotMain::weighted_urgency(InfantryBehaviour p_behaviour) const {
	return behaviour_urgency(p_behaviour) * behaviour_urgencies_[size_t(p_behaviour)];
}

}

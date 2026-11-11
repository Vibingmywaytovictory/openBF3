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
	behaviours_[size_t(InfantryBehaviour::MedicAssist)] =
			std::make_unique<BBMedicAssist>(curve(InfantryBehaviour::MedicAssist), body_);
	behaviours_[size_t(InfantryBehaviour::CallForMedic)] =
			std::make_unique<BBCallForMedic>(curve(InfantryBehaviour::CallForMedic), body_);
	behaviours_[size_t(InfantryBehaviour::Resupply)] =
			std::make_unique<BBResupply>(curve(InfantryBehaviour::Resupply), body_);
	std::vector<UrgencyGenerator *> generators;
	for (const std::unique_ptr<BotBehaviour> &b : behaviours_) {
		generators.push_back(b.get());
	}
	merger_ = std::make_unique<UrgencyMerger>(std::move(generators));
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
	const double now = double(tick) * sim::kTickSeconds;
	const bool unplanned = plan_invalid_ || current_ == nullptr || body_.senses() != decided_senses_;
	++stats_.decisions;
	++(unplanned ? stats_.unplanned : stats_.planned);
	calculate_mods();
	const int last = memory_.last_behaviour;
	if (plan_invalid_) {
		memory_.last_behaviour = -1;
		memory_.chosen_time = now;
	}
	std::array<float, kInfantryBehaviourCount> modifiers{};
	for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
		modifiers[i] = behaviour_urgencies_[i];
		if (!plan_invalid_ && last != -1) {
			modifiers[i] *= memory_.modify_behaviour_urgency(*behaviours_[i], now);
		}
		behaviours_[i]->calculate_urgency(this, modifiers[i]);
	}
	bool merge = plan_invalid_ || merger_->has_changed(this, kQuotientLow, kQuotientHigh);
	BotBehaviour *winner = nullptr;
	for (size_t attempt = 0; attempt <= kInfantryBehaviourCount; ++attempt) {
		if (merge) {
			merger_->update_active_urgency(this);
		}
		winner = static_cast<BotBehaviour *>(merger_->chosen_generator());
		float urgency = merger_->active_urgency(this);
		if (winner == nullptr) {
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
		winner->calculate_urgency(this, modifiers[size_t(winner->kind())]);
		merge = true;
		++stats_.refusals;
	}
	memory_.last_behaviour = int(winner->kind());
	memory_.chosen_time = now;
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

float BotMain::weighted_urgency(InfantryBehaviour p_behaviour) const {
	return behaviours_[size_t(p_behaviour)]->urgency_value();
}

}

#include "bf3/ai/bot/bot_main.h"

#include "bf3/ai/knowledge/information.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

#include <algorithm>
#include <vector>

namespace bf3::ai {

namespace {

inline constexpr double kRearmPeriod = 6.0;
inline constexpr float kRearmPercent = 0.125f;

}

BotMain::BotMain(uint32_t p_bot_id, int p_player_id, int p_side, InfantryBody &r_body, const BehaviourTuning &p_tuning,
		const float *p_personality) :
		bot_id_(p_bot_id), player_id_(p_player_id), side_(p_side), body_(r_body), tuning_(p_tuning),
		behaviour_count_(p_tuning.change ? kBotBehaviourCount : kInfantryBehaviourCount), plan_(r_body) {
	for (size_t i = 0; i < behaviour_count_; ++i) {
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
	if (tuning_.change) {
		behaviours_[size_t(InfantryBehaviour::Change)] =
				std::make_unique<BBChange>(curve(InfantryBehaviour::Change), body_);
	}
	std::vector<UrgencyGenerator *> generators;
	for (size_t i = 0; i < behaviour_count_; ++i) {
		generators.push_back(behaviours_[i].get());
	}
	merger_ = std::make_unique<UrgencyMerger>(std::move(generators));
	set_personality(p_personality);
	reset();
}

BotMain::~BotMain() = default;

void BotMain::set_personality(const float *p_weights) {
	for (size_t i = 0; i < behaviour_count_; ++i) {
		personality_[i] = p_weights != nullptr ? p_weights[i] : 1.0f;
	}
	calculate_mods();
}

void BotMain::calculate_mods() {
	for (size_t i = 0; i < behaviour_count_; ++i) {
		if (taking_orders_ && current_orders_ != nullptr && i < kInfantryBehaviourCount) {
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
	for (size_t i = 0; i < behaviour_count_; ++i) {
		behaviours_[i]->reset(this);
	}
	merger_->reset(this);
	current_ = nullptr;
	chosen_urgency_ = 0.0f;
	plan_.clear(InfantryActivity::Idle);
	plan_invalid_ = true;
	has_default_plan_ = false;
	if (communication_ != nullptr) {
		communication_->reset();
	}
	decided_senses_ = InfantrySenses{};
	ordered_behaviour_ = -1;
}

void BotMain::attach_communication(const RadioVoices &p_voices, RadioComm &r_hub) {
	communication_ = std::make_unique<CommunicationAgent>(p_voices, r_hub);
}

void BotMain::update_communication() {
	if (communication_ != nullptr) {
		communication_->update(double(body_.tick()) * sim::kTickSeconds, player_id_, side_);
	}
}

void BotMain::call_for_medic() {
	if (communication_ != nullptr) {
		communication_->call_for_medic(double(body_.tick()) * sim::kTickSeconds, true);
	}
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
	if (!plan_invalid_ && body_.senses() != decided_senses_) {
		has_default_plan_ = true;
	}
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
	std::array<float, kBotBehaviourCount> modifiers{};
	for (size_t i = 0; i < behaviour_count_; ++i) {
		modifiers[i] = behaviour_urgencies_[i];
		if (!plan_invalid_ && last != -1) {
			modifiers[i] *= memory_.modify_behaviour_urgency(*behaviours_[i], now);
		}
		behaviours_[i]->calculate_urgency(this, modifiers[i]);
	}
	bool merge = plan_invalid_ || merger_->has_changed(this, kQuotientLow, kQuotientHigh);
	BotBehaviour *winner = nullptr;
	for (size_t attempt = 0; attempt <= behaviour_count_; ++attempt) {
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
			has_default_plan_ = false;
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
	return 1;
}

void BotMain::plan_execution(double) {
	if (plan_invalid_) {
		return;
	}
	body_.begin_plan(plan_.activity());
	const bool executed = plan_.execute(this);
	const bool ok = body_.end_plan() && executed;
	if (!ok) {
		plan_invalid_ = true;
		body_.reset_controls();
		++stats_.plan_failures;
	}
}

void BotMain::update_player_action(bool p_stalled) {
	stalled_plan_ticks_ = p_stalled ? stalled_plan_ticks_ + 1 : 0;
}

void BotMain::rearm(const Information &p_equipment, const WeaponInfoManager &p_weapons, double p_time_increase) {
	if (man_down_) {
		return;
	}
	rearm_timer_ += p_time_increase;
	if (rearm_timer_ <= kRearmPeriod) {
		return;
	}
	IPIArmament *const armament = p_equipment.armament();
	if (armament != nullptr) {
		const std::vector<uint32_t> weapons = armament->weapons();
		for (const uint32_t handle : weapons) {
			if (handle == kNoHandle) {
				continue;
			}
			if (WeaponInfo *const weapon = p_weapons.information(handle)) {
				armament->rearm(*weapon, kRearmPercent, side_);
			}
		}
	}
	rearm_timer_ = 0.0;
}

bool BotMain::update_bot_vehicle() {
	plan_invalid_ = true;
	has_default_plan_ = false;
	plan_lock_ = false;
	return true;
}

bool BotMain::update_bot() {
	if (current_equipment_ == kNoHandle) {
		return false;
	}
	for (size_t i = 0; i < behaviour_count_; ++i) {
		behaviours_[i]->reset(this);
	}
	calculate_mods();
	merger_->update_active_urgency(this);
	memory_ = BotMemory{};
	return true;
}

void BotMain::reset_assigned_areas() {
	assigned_areas_[0] = -1;
	assigned_areas_[2] = -1;
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

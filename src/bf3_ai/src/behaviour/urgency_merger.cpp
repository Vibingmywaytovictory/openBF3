#include "bf3/ai/behaviour/urgency_merger.h"

#include <utility>

namespace bf3::ai {

UrgencyMerger::UrgencyMerger(std::vector<UrgencyGenerator *> p_generators) : generators_(std::move(p_generators)) {}

float UrgencyMerger::urgency(Bot *p_bot) {
	return max_generator_ != nullptr ? max_generator_->urgency(p_bot) : 0.0f;
}

float UrgencyMerger::active_urgency(Bot *p_bot) {
	return max_generator_ != nullptr ? max_generator_->active_urgency(p_bot) : 0.0f;
}

float UrgencyMerger::update_active_urgency(Bot *p_bot) {
	float best = 0.0f;
	max_generator_ = nullptr;
	for (UrgencyGenerator *const generator : generators_) {
		const float u = generator != nullptr ? generator->update_active_urgency(p_bot) : 0.0f;
		if (u > best) {
			best = u;
			max_generator_ = generator->chosen_generator();
		}
	}
	return best;
}

bool UrgencyMerger::has_changed_target(Bot *p_bot) {
	bool changed = true;
	for (size_t i = 0; i < generators_.size() && changed; ++i) {
		if (generators_[i] != nullptr) {
			changed = generators_[i]->has_changed_target(p_bot);
		}
	}
	return changed;
}

bool UrgencyMerger::quotient_changed(Bot *p_bot, float p_low, float p_high) {
	bool changed = true;
	for (size_t i = 0; i < generators_.size() && changed; ++i) {
		if (generators_[i] != nullptr) {
			changed = generators_[i]->quotient_changed(p_bot, p_low, p_high);
		}
	}
	return changed;
}

bool UrgencyMerger::has_changed(Bot *p_bot, float p_low, float p_high) {
	bool changed = false;
	for (size_t i = 0; i < generators_.size() && !changed; ++i) {
		if (generators_[i] != nullptr) {
			changed =
					generators_[i]->quotient_changed(p_bot, p_low, p_high) || generators_[i]->has_changed_target(p_bot);
		}
	}
	return changed;
}

UrgencyGenerator *UrgencyMerger::chosen_generator() {
	return max_generator_;
}

void UrgencyMerger::reset(Bot *p_bot) {
	for (UrgencyGenerator *const generator : generators_) {
		if (generator != nullptr) {
			generator->reset(p_bot);
		}
	}
}

int UrgencyMerger::size() const {
	return int(generators_.size());
}

}

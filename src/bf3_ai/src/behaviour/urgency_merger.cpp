#include "bf3/ai/behaviour/urgency_merger.h"

#include <utility>

namespace bf3::ai {

UrgencyMerger::UrgencyMerger(int p_count, const float *p_weights, std::vector<UrgencyGenerator *> p_generators) :
		count_(p_count), weights_(p_weights), generators_(std::move(p_generators)), chosen_(nullptr) {
	generators_.resize(size_t(count_ > 0 ? count_ : 0), nullptr);
}

float UrgencyMerger::urgency(Bot *p_bot) {
	chosen_ = nullptr;
	float best = 0.0f;
	for (size_t i = 0; i < generators_.size(); ++i) {
		UrgencyGenerator *const generator = generators_[i];
		if (generator == nullptr) {
			continue;
		}
		const float weighted = generator->urgency(p_bot) * (weights_ != nullptr ? weights_[i] : 1.0f);
		if (chosen_ == nullptr || weighted > best) {
			best = weighted;
			chosen_ = generator;
		}
	}
	return chosen_ != nullptr ? best : 0.0f;
}

float UrgencyMerger::active_urgency(Bot *p_bot) {
	return chosen_ != nullptr ? chosen_->active_urgency(p_bot) : 0.0f;
}

void UrgencyMerger::update_active_urgency(Bot *p_bot) {
	if (chosen_ != nullptr) {
		chosen_->update_active_urgency(p_bot);
	}
}

bool UrgencyMerger::has_changed_target(Bot *p_bot) {
	return chosen_ != nullptr && chosen_->has_changed_target(p_bot);
}

UrgencyGenerator *UrgencyMerger::chosen_generator() {
	return chosen_;
}

void UrgencyMerger::reset(Bot *p_bot) {
	for (UrgencyGenerator *const generator : generators_) {
		if (generator != nullptr) {
			generator->reset(p_bot);
		}
	}
	chosen_ = nullptr;
}

int UrgencyMerger::size() {
	return count_;
}

}

#pragma once

#include <vector>

namespace bf3::ai {

class Bot;

class UrgencyGenerator {
public:
	virtual ~UrgencyGenerator() = default;
	virtual float urgency(Bot *p_bot) = 0;
	virtual float active_urgency(Bot *p_bot) = 0;
	virtual void update_active_urgency(Bot *p_bot) = 0;
	virtual bool has_changed_target(Bot *p_bot) = 0;
	virtual UrgencyGenerator *chosen_generator() = 0;
	virtual void reset(Bot *p_bot) = 0;
	virtual int size() = 0;
};

class UrgencyMerger final : public UrgencyGenerator {
public:
	UrgencyMerger(int p_count, const float *p_weights, std::vector<UrgencyGenerator *> p_generators);

	float urgency(Bot *p_bot) override;
	float active_urgency(Bot *p_bot) override;
	void update_active_urgency(Bot *p_bot) override;
	bool has_changed_target(Bot *p_bot) override;
	UrgencyGenerator *chosen_generator() override;
	void reset(Bot *p_bot) override;
	int size() override;

private:
	int count_ = 0;
	const float *weights_ = nullptr;
	std::vector<UrgencyGenerator *> generators_;
	UrgencyGenerator *chosen_ = nullptr;
};

}

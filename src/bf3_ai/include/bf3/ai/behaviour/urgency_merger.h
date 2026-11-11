#pragma once

#include <vector>

namespace bf3::ai {

class Bot;

class UrgencyGenerator {
public:
	virtual ~UrgencyGenerator() = default;
	virtual float urgency(Bot *p_bot) = 0;
	virtual float active_urgency(Bot *p_bot) = 0;
	virtual float update_active_urgency(Bot *p_bot) = 0;
	virtual bool has_changed_target(Bot *p_bot) = 0;
	virtual bool quotient_changed(Bot *p_bot, float p_low, float p_high) = 0;
	virtual bool has_changed(Bot *p_bot, float p_low, float p_high) = 0;
	virtual UrgencyGenerator *chosen_generator() = 0;
	virtual void reset(Bot *p_bot) = 0;
};

class UrgencyMerger final : public UrgencyGenerator {
public:
	explicit UrgencyMerger(std::vector<UrgencyGenerator *> p_generators);

	float urgency(Bot *p_bot) override;
	float active_urgency(Bot *p_bot) override;
	float update_active_urgency(Bot *p_bot) override;
	bool has_changed_target(Bot *p_bot) override;
	bool quotient_changed(Bot *p_bot, float p_low, float p_high) override;
	bool has_changed(Bot *p_bot, float p_low, float p_high) override;
	UrgencyGenerator *chosen_generator() override;
	void reset(Bot *p_bot) override;
	int size() const;

private:
	std::vector<UrgencyGenerator *> generators_;
	UrgencyGenerator *max_generator_ = nullptr;
};

}

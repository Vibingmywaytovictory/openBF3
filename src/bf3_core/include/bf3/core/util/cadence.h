#pragma once

#include <cmath>

namespace bf3::core {

class Cadence {
public:
	explicit Cadence(double p_period_s) : period_s_(p_period_s) {}

	bool step(double p_delta_s) {
		owed_s_ += p_delta_s;
		if (owed_s_ < period_s_) {
			return false;
		}
		owed_s_ = std::fmod(owed_s_ - period_s_, period_s_);
		return true;
	}

private:
	double period_s_;
	double owed_s_ = 0.0;
};

}

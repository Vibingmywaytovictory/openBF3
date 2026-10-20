#pragma once

#include <chrono>

namespace bf3::host::detail {

class TickTimer {
public:
	TickTimer();
	~TickTimer();
	TickTimer(const TickTimer &) = delete;
	TickTimer &operator=(const TickTimer &) = delete;
	void sleep_until(std::chrono::steady_clock::time_point p_deadline);

private:
	void *timer_ = nullptr;
};

}

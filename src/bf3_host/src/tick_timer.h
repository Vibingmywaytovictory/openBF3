#pragma once
// Sleeps until an absolute steady_clock deadline with ~0.5 ms accuracy on Windows (high-resolution
// waitable timer; plain sleep_until can overshoot by a whole 15.6 ms scheduler quantum).

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

} // namespace bf3::host::detail

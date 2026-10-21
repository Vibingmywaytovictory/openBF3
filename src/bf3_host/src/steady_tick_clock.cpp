#include "bf3/host/tick_clock.h"

#include "tick_timer.h"

#include <chrono>

namespace bf3::host {

namespace {

class SteadyTickClock final : public TickClock {
public:
	int64_t now_ns() override {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count();
	}

	void sleep_until_ns(int64_t p_deadline_ns) override {
		timer_.sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(p_deadline_ns)));
	}

private:
	detail::TickTimer timer_;
};

}

std::shared_ptr<TickClock> make_steady_tick_clock() {
	return std::make_shared<SteadyTickClock>();
}

}

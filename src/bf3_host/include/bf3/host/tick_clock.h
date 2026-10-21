#pragma once

#include <cstdint>
#include <memory>

namespace bf3::host {

class TickClock {
public:
	virtual ~TickClock() = default;
	virtual int64_t now_ns() = 0;
	virtual void sleep_until_ns(int64_t p_deadline_ns) = 0;
};

std::shared_ptr<TickClock> make_steady_tick_clock();

}

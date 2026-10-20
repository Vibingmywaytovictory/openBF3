#include "tick_timer.h"

#include <windows.h>

#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace bf3::host::detail {

TickTimer::TickTimer() {
	timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

TickTimer::~TickTimer() {
	if (timer_ != nullptr) {
		CloseHandle(static_cast<HANDLE>(timer_));
	}
}

void TickTimer::sleep_until(std::chrono::steady_clock::time_point p_deadline) {
	using namespace std::chrono;
	const auto now = steady_clock::now();
	if (p_deadline <= now) {
		return;
	}
	if (timer_ == nullptr) {
		std::this_thread::sleep_until(p_deadline);
		return;
	}
	const int64_t ticks_100ns = duration_cast<nanoseconds>(p_deadline - now).count() / 100;
	LARGE_INTEGER due;
	due.QuadPart = -ticks_100ns;
	if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, nullptr, 0)) {
		WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
	} else {
		std::this_thread::sleep_until(p_deadline);
	}
}

}

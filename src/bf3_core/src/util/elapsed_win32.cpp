#include "bf3/core/util/elapsed.h"

#include <windows.h>

#include <cstdint>

namespace bf3::core {

double thread_cpu_seconds() {
	FILETIME created, exited, kernel, user;
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
		return 0.0;
	}
	const auto ticks = [](const FILETIME &p_t) { return (uint64_t(p_t.dwHighDateTime) << 32) | p_t.dwLowDateTime; };
	return double(ticks(kernel) + ticks(user)) * 1e-7;
}

}

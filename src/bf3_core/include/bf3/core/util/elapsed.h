#pragma once

#include <chrono>

namespace bf3::core {

inline double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

inline double ms_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p_start).count();
}

double thread_cpu_seconds();

}

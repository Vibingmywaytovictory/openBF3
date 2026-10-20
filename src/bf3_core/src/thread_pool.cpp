#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace bf3::core {

unsigned parallel_lanes(size_t p_count, unsigned p_max_lanes) {
	unsigned lanes = std::max(1u, std::thread::hardware_concurrency());
	if (p_max_lanes != 0) {
		lanes = std::min(lanes, p_max_lanes);
	}
	return static_cast<unsigned>(std::min<size_t>(lanes, std::max<size_t>(p_count, 1)));
}

void parallel_for(size_t p_count, const std::function<void(size_t)> &p_work, unsigned p_max_lanes) {
	const unsigned lanes = parallel_lanes(p_count, p_max_lanes);
	if (lanes <= 1) {
		for (size_t i = 0; i < p_count; ++i) {
			p_work(i);
		}
		return;
	}
	std::atomic<size_t> next{ 0 };
	auto lane = [&]() {
		for (;;) {
			const size_t i = next.fetch_add(1, std::memory_order_relaxed);
			if (i >= p_count) {
				return;
			}
			p_work(i);
		}
	};
	std::vector<std::thread> threads;
	threads.reserve(lanes - 1);
	for (unsigned t = 1; t < lanes; ++t) {
		threads.emplace_back(lane);
	}
	lane();
	for (std::thread &t : threads) {
		t.join();
	}
}

}

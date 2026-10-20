#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace bf3::core {

unsigned parallel_lanes(size_t p_count, unsigned p_max_lanes = 0);

void parallel_for(size_t p_count, const std::function<void(size_t)> &p_work, unsigned p_max_lanes = 0);

template <class R, class F>
std::vector<R> parallel_map(size_t p_count, F &&p_work, unsigned p_max_lanes = 0) {
	std::vector<std::optional<R>> slots(p_count);
	parallel_for(p_count, [&](size_t i) { slots[i].emplace(p_work(i)); }, p_max_lanes);
	std::vector<R> out;
	out.reserve(p_count);
	for (std::optional<R> &slot : slots) {
		out.push_back(std::move(*slot));
	}
	return out;
}

}

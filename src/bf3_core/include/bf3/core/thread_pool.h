#pragma once
// Data-parallel helpers on std::thread (no Godot). Ported from IW4L asset_transport::bf3
// (mount.rs parallel_map): up to hardware_concurrency lanes pull indices from an atomic counter;
// results keep the input order.

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace bf3::core {

// Lanes used for p_count items (1 when p_count <= 1 or the machine reports one thread).
unsigned parallel_lanes(size_t p_count, unsigned p_max_lanes = 0);

// Calls p_work(i) for every i in [0, p_count), on up to p_max_lanes threads (0 = all cores).
// The calling thread is one of the lanes. Blocks until every item is done.
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

} // namespace bf3::core
